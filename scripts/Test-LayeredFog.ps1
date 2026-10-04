# Installed original Everon. Root runtime owner invokes through with-game-lock.sh.
[CmdletBinding()]
param(
 [ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit='',
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='layered-asl-fog',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [double]$X=6532,[double]$Z=6466,[ValidateRange(0.05,0.8)][double]$WeatherFog=0.45,
 [switch]$Measure,[ValidateRange(15,120)][int]$MeasureDeadlineSeconds=90,
 [double]$Exposure,[switch]$TerrainMode,[switch]$CompactBand,[switch]$MovingViews,[switch]$SelfTest
)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($v){$n=[double]::Parse([string]$v,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite receipt.';return $n}
function Fmt($v){return (Num $v).ToString('R',$culture)}
function Exposure-Settings([bool]$supplied,$value){
 if(!$supplied){return @{}}
 $fixed=Num $value;Require ($fixed -ge .0001 -and $fixed -le 16) 'Fixed exposure must be finite within 0.0001..16.'
 # Manual base exposure also disables the automatic time-of-day grade.
 # Adaptive eye exposure is an independent multiplier and must be disabled too.
 return @{WGR_EXPOSURE=(Fmt $fixed);WGR_AUTO_EXPOSURE='0';WGR_TONEMAP='1'}
}
$exposureSupplied=$PSBoundParameters.ContainsKey('Exposure')
$exposureSettings=Exposure-Settings $exposureSupplied $Exposure
Require (!$CompactBand -or $TerrainMode) 'CompactBand requires TerrainMode.'
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete string.';return $value.Substring(1,$value.Length-2).Replace('""','"')};return ConvertFrom-Json $value -NoEnumerate}
function Timing-Sample($sample){
 # Missing/negative counters mean unavailable. A real published zero remains zero.
 function Region([string]$name){
  $rows=@($sample.regions|Where-Object{$_.name -ceq $name})
  Require ($rows.Count -le 1) ('Duplicate timer: '+$name)
  if($rows.Count -eq 0){return @{available=$false;reason='timer absent';milliseconds=$null}}
  $row=$rows[0];$ms=Num $row.gpuMs
  if($sample.gpuTimestampsAvailable -ne $true -or $ms -lt 0){return @{available=$false;reason='timestamps or published region unavailable';milliseconds=$null;raw=$row}}
  return @{available=$true;milliseconds=$ms;index=$row.index;containedBy=$row.containedBy;container=$row.container;cpuMs=$row.cpuMs}
 }
 return @{gpuFrame=(Region 'GPU frame total');fogTransport=(Region 'Layered ASL fog: transport');completedCpuFrames=$sample.completedCpuFrames}
}
function Assert-Weather($weather,[double]$fog){Require ([Math]::Abs((Num $weather.fog)-$fog) -lt .001 -and (Num $weather.rain) -lt .001 -and (Num $weather.overcast) -lt .001) 'Weather changed during captured view.'}
function Assert-Source($source,$reference){Require ($source.worldName -ceq $reference.worldName -and $source.heightRevision -eq $reference.heightRevision -and $source.grid -eq $reference.grid -and $source.count -eq 0) 'Native source changed during captured view.'}
function Frame-Advance($start,$end){
 $a=Num $start;$b=Num $end
 Require ($a -ge 0 -and $b -ge $a -and $a -eq [Math]::Floor($a) -and $b -eq [Math]::Floor($b)) 'Rendered frame counter missing, reset or wrapped.'
 return ($b-$a)
}
if($SelfTest){
 function Refuses([scriptblock]$action){$refused=$false;try{& $action|Out-Null}catch{$refused=$true};Require $refused 'Unsafe receipt accepted.'}
 $sample=[pscustomobject]@{gpuTimestampsAvailable=$true;completedCpuFrames=256;regions=@([pscustomobject]@{name='GPU frame total';gpuMs=12.5;index=30},[pscustomobject]@{name='Layered ASL fog: transport';gpuMs=.35;index=88})}
 $actual=Timing-Sample $sample;Require ($actual.gpuFrame.milliseconds -eq 12.5 -and $actual.fogTransport.milliseconds -eq .35) 'Measured timers not retained.'
 $sample.regions[1].gpuMs=-1;Require (!(Timing-Sample $sample).fogTransport.available) 'Unavailable region became zero.'
 $sample.regions[1].gpuMs=0;Require ((Timing-Sample $sample).fogTransport.available -and (Timing-Sample $sample).fogTransport.milliseconds -eq 0) 'Published zero discarded.'
 $sample.gpuTimestampsAvailable=$false;Require (!(Timing-Sample $sample).gpuFrame.available) 'Unavailable timestamps accepted.'
 $sample.gpuTimestampsAvailable=$true;$sample.regions[0].gpuMs=[double]::NaN;Refuses {Timing-Sample $sample}
 $sample.regions=@();Require (!(Timing-Sample $sample).fogTransport.available) 'Missing timer became zero.'
 $sample.regions=@(@{name='GPU frame total';gpuMs=1},@{name='GPU frame total';gpuMs=2});Refuses {Timing-Sample $sample}
 Require ((Frame-Advance 100 356) -eq 256) 'Rendered warmup count incorrect.';Refuses {Frame-Advance 356 100};Refuses {Frame-Advance 0 1.5}
 $weather=@{fog=.45;rain=0;overcast=0};Assert-Weather $weather .45;$weather.rain=.1;Refuses {Assert-Weather $weather .45}
 $source=@{worldName='eden.wrp';heightRevision=3;grid=256;count=0};Assert-Source $source $source
 Refuses {Assert-Source (@{worldName='noe.wrp';heightRevision=3;grid=256;count=0}) $source}
 Refuses {Assert-Source (@{worldName='eden.wrp';heightRevision=4;grid=256;count=0}) $source}
 Require ((Exposure-Settings $false .15).Count -eq 0) 'Unset exposure changed default grade.'
 $fixed=Exposure-Settings $true .15;Require ($fixed.WGR_EXPOSURE -ceq '0.15' -and $fixed.WGR_AUTO_EXPOSURE -ceq '0' -and $fixed.WGR_TONEMAP -ceq '1') 'Fixed Hable/adaptive exposure controls incorrect.'
 Refuses {Exposure-Settings $true 0};Refuses {Exposure-Settings $true 17};Refuses {Exposure-Settings $true ([double]::NaN)};Refuses {Exposure-Settings $true ([double]::PositiveInfinity)}
 Write-Output 'Layered fog timing/exposure helper PASS: real/zero/unavailable, nonfinite/duplicate, counter, weather/source and finite fixed-exposure refusals. No game or GPU.';return
}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp -match [regex]::Escape($ExpectedCommit)) 'Installed commit differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$file=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length}})}
}
Require ([bool]$ExpectedCommit) 'Supply the installed ExpectedCommit.'
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/layered-fog/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'));New-Item -ItemType Directory -Force $taskOut|Out-Null
$keys=@('POSEIDON_USER_DIR','WGR_LAYERED_FOG','WGR_LAYERED_FOG_TRACE','WGR_FOG_LAYER0','WGR_FOG_LAYER1','WGR_FOG_TERRAIN','WGR_FOG_PATCH','WGR_WEATHER_COVER','WGR_WEATHER_COVER_FAR','WGR_GRASS','WGR_TEMPORAL','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_TEST_RAIN','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_CLOUD_COVERAGE')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;terrainMode=[bool]$TerrainMode;movingRenderCamera=[bool]$MovingViews;measure=[bool]$Measure;measureWarmupRenderedFrames=256;measureDeadlineSeconds=$MeasureDeadlineSeconds;
 exposureControl=@{supplied=$exposureSupplied;environment=$exposureSettings;scope='Explicit startup environment controls when supplied: fixed base exposure, adaptive exposure OFF, manual Hable time-of-day grade. Otherwise normal defaults. Configuration receipt, not same-frame GPU uniform readback. Bloom remains unchanged.'};
 arms=@();scope='Actual original Everon and authoritative weather, same XYZ camera requests, unchanged native source/ranges at equal weather. Encoded trace is CPU command readiness. Per-view frame_performance receipts contain latest asynchronous published GPU timers, without sampled GPU frame ID or exact screenshot-frame attribution. Frame total is inclusive; fog transport excludes background/consumer/weather-map costs. No timer summation, steady statistical benchmark, uncontended cost certification, visual acceptance or default promotion claimed. Terrain mode is an explicit experiment; no quality/cost acceptance is claimed.'}
$p=$null;$client=$null;$ground=$null;$layerArgs=$null
function Send($command){
 Require (!$p.HasExited) 'Owned game exited unexpectedly.';$wire=$command|ConvertTo-Json -Depth 8 -Compress;$wire|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl');Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Logs{return @(@($log,$stderr,$stdout)|Where-Object{Test-Path -LiteralPath $_}|ForEach-Object{Get-Content -LiteralPath $_})}
try{
 foreach($arm in @(@{name='off';on='0';fog=$WeatherFog},@{name='on';on='1';fog=$WeatherFog},@{name='on-zero';on='1';fog=0},@{name='off-zero';on='0';fog=0})){
  Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Foreign game appeared between arms.'
  $armOut=Join-Path $taskOut $arm.name;$profile=Join-Path $armOut 'user';New-Item -ItemType Directory -Force $profile|Out-Null
  [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
  foreach($key in $keys){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
  $settings=@{POSEIDON_USER_DIR=$profile;WGR_LAYERED_FOG=$arm.on;WGR_LAYERED_FOG_TRACE='1';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_FAR='1';WGR_GRASS='0';WGR_TEMPORAL='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
  if(!$TerrainMode){$settings.WGR_FOG_TERRAIN='0,0,0,0';$settings.WGR_FOG_PATCH='0,0,0,0'}
  foreach($key in $exposureSettings.Keys){$settings[$key]=$exposureSettings[$key]}
  if($layerArgs){$settings.WGR_FOG_LAYER0=$layerArgs[0];$settings.WGR_FOG_LAYER1=$layerArgs[1]}
  if($CompactBand){$settings.WGR_FOG_LAYER0='1,5,0.35,0.018';$settings.WGR_FOG_LAYER1='35,50,3,0';$settings.WGR_FOG_TERRAIN='1,0,1,150';$settings.WGR_FOG_PATCH='0.35,250,70,30'}
  foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
  $log=Join-Path $armOut 'engine.log';$stdout=Join-Path $armOut 'stdout.log';$stderr=Join-Path $armOut 'stderr.log'
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden'
  $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
  $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
  $until=[DateTime]::UtcNow.AddSeconds(120)
  do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady') -cne 'OK'){Require ([DateTime]::UtcNow -lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 300}
  Exec ('player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog '+(Fmt $arm.fog)+';setDate [1985,6,21,12,0];setAccTime 1')
  # Weather setters and visibility-range updates run at distinct engine boundaries.
  # Observe actual rendered progress and consecutive stable range samples before
  # freezing; later cross-arm range equality remains mandatory.
  $settleStart=Send @{cmd='query';what='play_state'}
  $until=[DateTime]::UtcNow.AddSeconds(30);$priorWeather=$null;$stableWeather=0
  do{
   Require ([DateTime]::UtcNow -lt $until) 'Actual weather/ranges did not settle.'
   $weather=Send @{cmd='weather_visibility'};$settleNow=Send @{cmd='query';what='play_state'}
   $matches=[Math]::Abs((Num $weather.fog)-$arm.fog) -lt .001 -and (Num $weather.overcast) -lt .001 -and (Num $weather.rain) -lt .001
   $rangesStable=$null -ne $priorWeather
   if($rangesStable){foreach($range in @('terrainRange','baseRange','objectRange','tacticalVisibility')){if([Math]::Abs((Num $weather.$range)-(Num $priorWeather.$range)) -ge .001){$rangesStable=$false}}}
   if($matches -and $rangesStable -and (Frame-Advance $settleStart.frame $settleNow.frame) -ge 30){$stableWeather++}else{$stableWeather=0}
   $priorWeather=$weather
   if($stableWeather -ge 3){break};Start-Sleep -Milliseconds 300
  }while($true)
  Exec 'setAccTime 0'
  $source=Send @{cmd='dev_cave_editor';action='state';x=$X;z=$Z;y=1000}
  Require ($source.worldName.Replace('\','/').ToLowerInvariant() -match '(^|/)eden\.wrp$' -and $source.count -eq 0) 'Not untouched original Everon.'
  $actualGround=Num (Eval ('triTerrainHeight ['+(Fmt $X)+','+(Fmt $Z)+']'))
  if($null -eq $ground){
   $ground=$actualGround
   $lower=(@(($ground-3),($ground+18),3,.008)|ForEach-Object{Fmt $_}) -join ','
   $upper=(@(($ground+35),($ground+50),3,.004)|ForEach-Object{Fmt $_}) -join ','
   $layerArgs=@($lower,$upper)
   if($TerrainMode){$layerArgs=@('-3,18,3,0.008','35,50,3,0.004')}
   if($CompactBand){$layerArgs=@('1,5,0.35,0.018','35,50,3,0')}
   $baselineSource=$source;$baselineWeather=$weather;$result.profile=@{siteXZ=@($X,$Z);nativeY=$ground;layers=$layerArgs;terrainMode=[bool]$TerrainMode;policy='Legacy-density mode explicitly zeros new controls and freezes world-ASL slabs from actual baseline height. Terrain mode uses AGL slabs with actual C++ startup terrain/patch defaults; trace records packed controls. OFF scout used built-in profiles; disabled optics are independent of profile.'}
  }else{Require ([Math]::Abs($actualGround-$ground) -lt .0001 -and $source.worldName -ceq $baselineSource.worldName -and $source.heightRevision -eq $baselineSource.heightRevision -and $source.grid -eq $baselineSource.grid) 'Native source changed between arms.'}
  if($arm.fog -gt 0){foreach($range in @('terrainRange','baseRange','objectRange','tacticalVisibility')){Require ([Math]::Abs((Num $weather.$range)-(Num $baselineWeather.$range)) -lt .001) ('Existing '+$range+' changed at identical weather.')}}
  $receipt=@{name=$arm.name;settings=$settings;weather=$weather;source=$source;nativeY=$actualGround;views=@()};$result.arms+=,$receipt
  $views=@(@{name='ground';x=$X;z=$Z;y=($ground+2);nativeY=$ground},@{name='between';x=$X;z=$Z;y=($ground+26);nativeY=$ground},@{name='above';x=$X;z=$Z;y=($ground+75);nativeY=$ground})
  if($MovingViews){
   foreach($site in @(@{name='flight-left';x=($X-150);z=$Z;agl=75},@{name='flight-right';x=($X+150);z=($Z+150);agl=75},@{name='flight-high';x=($X+150);z=($Z+150);agl=150})){
    $native=Num (Eval ('triTerrainHeight ['+(Fmt $site.x)+','+(Fmt $site.z)+']'))
    $views+=@{name=$site.name;x=$site.x;z=$site.z;y=($native+$site.agl);nativeY=$native}
   }
  }
  $previousPose=$null
  foreach($view in $views){
   $pose=@($view.x,$view.y,$view.z,-.65,-.07,-.76)
   $movement=@()
   if($MovingViews -and $previousPose){
    # Bounded render-camera movement only: the simulation remains paused and
    # there is no physical helicopter, flight-cost or temporal-image assertion.
    foreach($step in 1..16){$t=$step/16.;$sample=@();foreach($i in 0..2){$sample+=($previousPose[$i]+($pose[$i]-$previousPose[$i])*$t)};$sample+=@(-.65,-.07,-.76)
     Require ((Eval ('triSetView ['+(($sample|ForEach-Object{Fmt $_}) -join ',')+']')) -ceq 'OK') 'Moving render-camera refused.'
     $progress=Send @{cmd='query';what='play_state'};$movement+=@{requested=$sample;frame=$progress.frame};Start-Sleep -Milliseconds 60
    }
   }
   $previousPose=$pose
   Require ((Eval ('triSetView ['+(($pose|ForEach-Object{Fmt $_}) -join ',')+']')) -ceq 'OK') 'Render-only camera refused.'
   $warmupStart=Send @{cmd='query';what='play_state'};$warmupClock=[Diagnostics.Stopwatch]::StartNew()
   Start-Sleep -Seconds 4
   if($Measure){
    do{
     $warmupEnd=Send @{cmd='query';what='play_state'};$advance=Frame-Advance $warmupStart.frame $warmupEnd.frame
     Require ($warmupClock.Elapsed.TotalSeconds -le $MeasureDeadlineSeconds) '256 rendered-frame measurement warmup deadline.'
     if($advance -ge 256){break};Start-Sleep -Milliseconds 300
    }while($true)
   }else{$warmupEnd=Send @{cmd='query';what='play_state'};$advance=Frame-Advance $warmupStart.frame $warmupEnd.frame}
   $warmupClock.Stop()
   $weatherBefore=Send @{cmd='weather_visibility'};Assert-Weather $weatherBefore $arm.fog
   $sourceBefore=Send @{cmd='dev_cave_editor';action='state';x=$view.x;z=$view.z;y=1000};Assert-Source $sourceBefore $baselineSource
   $nativeBefore=Num (Eval ('triTerrainHeight ['+(Fmt $view.x)+','+(Fmt $view.z)+']'));Require ([Math]::Abs($nativeBefore-$view.nativeY) -lt .0001) 'Captured site native height changed.'
   $performanceBefore=Send @{cmd='frame_performance'};$captureBefore=Send @{cmd='query';what='play_state'}
   $path=Join-Path $armOut ($view.name+'.png');$captureStartedUtc=[DateTime]::UtcNow.ToString('o');$null=Send @{cmd='screenshot';path=$path}
   $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow -lt $until) 'Screenshot deadline.';Start-Sleep -Milliseconds 150}
   Require ((Get-Item -LiteralPath $path).Length -gt 4096) 'Capture is empty.'
   $rows=@(Logs|Where-Object{$_ -match '\[wgr\] layered fog:.*camera=\['}|Select-Object -Last 8)
   $matching=@($rows|Where-Object{$_ -match 'weather=([-0-9.]+).*camera=\[([-0-9.eE+]+), ([-0-9.eE+]+), ([-0-9.eE+]+)' -and [Math]::Abs((Num $Matches[1])-$arm.fog) -lt .001 -and [Math]::Abs((Num $Matches[2])-$view.x) -lt .03 -and [Math]::Abs((Num $Matches[3])-$view.y) -lt .03 -and [Math]::Abs((Num $Matches[4])-$view.z) -lt .03})
   Require ($matching.Count -gt 0) 'No actual encoded-camera/weather trace at captured view.'
   if($arm.on -eq '1' -and $arm.fog -gt 0){Require ([bool]($matching|Where-Object{$_ -match 'encoded=true'})) 'Current source/physical coverage never admitted fog.'}else{Require (![bool]($matching|Where-Object{$_ -match 'encoded=true'})) 'Disabled/zero-weather control encoded fog.'}
   $captureAfter=Send @{cmd='query';what='play_state'};$performanceAfter=Send @{cmd='frame_performance'}
   $viewWeather=Send @{cmd='weather_visibility'};Assert-Weather $viewWeather $arm.fog
   $sourceAfter=Send @{cmd='dev_cave_editor';action='state';x=$view.x;z=$view.z;y=1000};Assert-Source $sourceAfter $baselineSource
   $nativeAfter=Num (Eval ('triTerrainHeight ['+(Fmt $view.x)+','+(Fmt $view.z)+']'));Require ([Math]::Abs($nativeAfter-$nativeBefore) -lt .0001) 'Native site changed across screenshot.'
   $timing=Timing-Sample $performanceAfter
   foreach($field in @('renderWidth','renderHeight','outputWidth','outputHeight','msaaSamples','activeUpscaler')){
    Require ($null -ne $performanceAfter.$field -and $performanceBefore.$field -eq $performanceAfter.$field) ('Render configuration changed or unavailable: '+$field)
    if($Measure -and $arm.name -ne 'off'){
     $reference=($result.arms[0].views|Where-Object{$_.name -ceq $view.name}).renderConfiguration
     Require ($null -ne $reference -and $reference[$field] -eq $performanceAfter.$field) ('Render configuration differs across measured arms: '+$field)
    }
   }
   $renderConfiguration=@{};foreach($field in @('renderWidth','renderHeight','outputWidth','outputHeight','msaaSamples','activeUpscaler')){$renderConfiguration[$field]=$performanceAfter.$field}
   if($Measure){Require ((Num $performanceAfter.completedCpuFrames) -ge 256) 'CPU profiler ring is not populated.';Require ($timing.gpuFrame.available -and $timing.gpuFrame.milliseconds -gt 0) 'Measured GPU frame total unavailable.'}
   $captureFrameAdvance=Frame-Advance $captureBefore.frame $captureAfter.frame
   $timingPath=Join-Path $armOut ($view.name+'.frame-performance.json')
   $timingReceipt=@{schema='layered-fog-published-timing-v1';cameraXYZDirectionXYZ=$pose;requestedUtc=$captureStartedUtc;png=$path;pngSha256=(Get-FileHash -LiteralPath $path).Hash;exposureControl=$result.exposureControl;
    renderedWarmup=@{startFrame=$warmupStart.frame;endFrame=$warmupEnd.frame;advance=$advance;seconds=$warmupClock.Elapsed.TotalSeconds;measured=[bool]$Measure};
    screenshotRequestWindow=@{beforeFrame=$captureBefore.frame;afterFrame=$captureAfter.frame;advance=$captureFrameAdvance};weatherBefore=$weatherBefore;weatherAfter=$viewWeather;sourceBefore=$sourceBefore;sourceAfter=$sourceAfter;nativeBefore=$nativeBefore;nativeAfter=$nativeAfter;
    timingBefore=$performanceBefore;timingAfter=$performanceAfter;sampledGpu=$timing;trace=$matching;renderCameraMovement=$movement;
    scope='Fresh per-view harness reads, not legacy capture-metrics JSON. GPU sample frame identity unavailable; camera/weather/source are bracket controls and matching CPU encoding trace, not same-frame GPU proof. Rendered frame count is FinishDraw, CPU count is a capped main-loop ring. Single inclusive whole-frame sample and transport-only timer; no sum, statistical distribution, steady CPU-ring ownership or uncontended budget acceptance.'}
   $timingReceipt|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $timingPath
   $receipt.views+=@{name=$view.name;cameraXYZDirectionXYZ=$pose;renderCameraMovement=$movement;nativeYAtSite=$view.nativeY;path=$path;pngSha256=$timingReceipt.pngSha256;trace=$matching;weather=$viewWeather;renderConfiguration=$renderConfiguration;timingReceipt=$timingPath;timingSha256=(Get-FileHash -LiteralPath $timingPath).Hash;sampledGpu=$timing}

  }
  $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000) -and $p.ExitCode -eq 0) 'Normal game exit failed.';$receipt.exit=$p.ExitCode
  Require (!(Logs|Select-String -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION')) 'Renderer failure logged.'
  $client.Dispose();$client=$null;$p=$null
 }
 $after=Pair;Require (($after|ConvertTo-Json -Depth 6 -Compress) -ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed.'
 $result.passed=$true;Write-Host "Installed layered fog functional controls PASS; image judgement pending: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p -and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null -eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
