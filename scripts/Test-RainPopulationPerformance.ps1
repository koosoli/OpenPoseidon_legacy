<# Camera-altitude RainVolume population/CPU-phase diagnostic; no helicopter-flight claim.
Invoke with-game-lock.sh. Forced uses the existing exact POSEIDON_TEST_RAIN=1 hook;
Natural requires a build with POSEIDON_RAIN_TRACE=1 and follows ordinary weather. #>
[CmdletBinding()]
param([ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
 [ValidateSet('Forced','Natural')][string]$Mode='Forced',
 [ValidateRange(20,30)][int]$MeasureSeconds=25,
 [ValidateRange(10,30)][int]$WarmupSeconds=15,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='rain-altitude-phase',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$SelfTest)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
$helper=Join-Path $PSScriptRoot 'Test-RainWaterFineRuntime.ps1'
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($helper,[ref]$tokens,[ref]$errors)
Require ($errors.Count -eq 0) 'Pure helper source does not parse.'
foreach($name in @('Number','Decode-Eval','Restore-Environment','Complete-LogLines','Read-CompleteLogLines')){
 $fn=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
 Require ($fn.Count -eq 1) ('Missing pure helper '+$name);Invoke-Expression $fn[0].Extent.Text
}
function Median($values){$v=@($values|Sort-Object);Require ($v.Count -gt 0) 'No phase samples.';$i=[int][Math]::Floor($v.Count/2);if($v.Count%2){return [double]$v[$i]};return ([double]$v[$i-1]+[double]$v[$i])/2}
function Rain-Comparable($weather){
 foreach($name in @('rain','liquidRain','particleDensity')){Require ($null -ne $weather.$name) ('Missing actual weather '+$name);$n=[double](Number $weather.$name);Require ($n -ge 0 -and $n -le 1) ('Invalid actual weather '+$name)}
 Require ($weather.particleSnowflakes -is [bool] -and !$weather.particleSnowflakes -and
  $weather.liquidRain -eq [Math]::Max($weather.rain,$weather.particleDensity) -and $weather.liquidRain -ge .95) 'Dense actual liquid rain >=.95 is required; request alone is insufficient.'
}
function Parse-RainRows([string[]]$lines){
 $grammar='RainVolume: drops=(\d+) drawn=(\d+) decals=(\d+) spawned=(\d+) covered=(\d+) roof=(\d+) indoor=(\d+) probe=(\d+) ground=(\d+) buildings=(\d+) sweeps=(\d+) wind=([0-9.eE+-]+)m/s@([0-9.eE+-]+)deg lean=([0-9.eE+-]+)deg spawn=([0-9.eE+-]+)us gather=([0-9.eE+-]+)us sim=([0-9.eE+-]+)us draw=([0-9.eE+-]+)us(?: volumeCulls=(\d+) capCulls=(\d+) limit=(\d+))?\s*$'
 $rows=@();$fields=@('drops','drawn','decals','spawned','covered','roof','indoor','probe','ground','buildings','sweeps','windSpeed','windDirection','lean','spawnUs','gatherUs','simUs','drawUs')
 foreach($line in $lines){if($line.Contains('RainVolume:')){
  Require ($line -cmatch $grammar) 'Unknown/incomplete RainVolume trace grammar.';$matched=$Matches;$row=[ordered]@{line=$line}
  for($i=0;$i -lt $fields.Count;$i++){$n=[double](Number $matched[$i+1]);Require ($n -ge 0 -or $fields[$i] -in @('windDirection','lean')) 'Negative particle count/time.';$row[$fields[$i]]=$n}
  foreach($newField in @(@('volumeCulls',19),@('capCulls',20),@('limit',21))){if($matched[$newField[1]]){$row[$newField[0]]=[double](Number $matched[$newField[1]])}}
  Require ($row.drawn -le $row.drops) 'Drawn drops exceed live population.';$rows+=,$row
 }};return $rows
}
function Summary($rows){
 Require ($rows.Count -ge 15) 'At least15 complete once-per-simulation-second samples required.';$s=[ordered]@{samples=$rows.Count}
 foreach($field in @('drops','drawn','spawned','covered','roof','indoor','probe','ground','buildings','sweeps','spawnUs','gatherUs','simUs','drawUs')){
  $v=@($rows|ForEach-Object{$_[$field]});$s[$field]=@{median=(Median $v);min=($v|Measure-Object -Minimum).Minimum;max=($v|Measure-Object -Maximum).Maximum}
 };return $s
}
if($SelfTest){
 $good='RainVolume: drops=100 drawn=20 decals=0 spawned=4 covered=0 roof=0 indoor=0 probe=0 ground=3 buildings=2 sweeps=50 wind=0.0m/s@90deg lean=0deg spawn=3us gather=5us sim=9us draw=6us'
 $r=@(Parse-RainRows @($good));Require ($r.Count -eq 1 -and $r[0].drops -eq 100 -and $r[0].drawUs -eq 6) 'Trace parser changed.'
 $pair=@(Parse-RainRows @($good,$good));Require ($pair.Count -eq 2) 'Trace rows were nested/truncated.'
 function Refuses([scriptblock]$body){$failed=$false;try{& $body|Out-Null}catch{$failed=$true};Require $failed 'Unsafe fixture accepted.'}
 Refuses {Parse-RainRows @($good.Replace('drawn=20','drawn=101'))};Refuses {Parse-RainRows @($good.Replace('sim=9us','sim=NaNus'))};Refuses {Summary @($r[0])}
 $w=@{rain=1;liquidRain=1;particleDensity=1;particleSnowflakes=$false};Rain-Comparable $w;$w.rain=.8;$w.liquidRain=1;Rain-Comparable $w;$w.liquidRain=.8;Refuses {Rain-Comparable $w}
 $w.rain=1;$w.liquidRain=1;$w.particleSnowflakes=$true;Refuses {Rain-Comparable $w}
 Require ((Median @(1,3,5,9)) -eq 4) 'Median changed.'
 Write-Output 'Rain population helpers PASS; no game launched.';return
}
Require ([bool]$ExpectedCommit) 'Supply exact installed -ExpectedCommit.';Require ([bool]$env:LOCK_OWNER) 'Invoke through with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
function Pair{
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ($stamp -match ('(?i)\b'+[regex]::Escape($ExpectedCommit)+'[a-f0-9]*\b')) 'Installed source differs.'
 return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
function Same-Pair($a,$b){Require ($a.stamp -ceq $b.stamp -and $a.exe -ceq $b.exe -and $a.dll -ceq $b.dll) 'Installed pair changed.'}
$before=Pair;$campaign=Join-Path $root ('build/rain-population/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6));New-Item -ItemType Directory -Force $campaign|Out-Null
$settings=@{POSEIDON_RAIN_TRACE='1';POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_GRASS='0';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_LOD_GOVERNOR_RANGE='1';POSEIDON_VSYNC='0'}
if($Mode -ceq 'Forced'){$settings.POSEIDON_TEST_RAIN='1'}
$clear=@('POSEIDON_TEST_RAIN','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','WGR_CLOUD_COVERAGE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLE_WETNESS')
$keys=@(@($settings.Keys)+$clear+@('POSEIDON_USER_DIR')|Select-Object -Unique);$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);installed=$before;lockOwner=$env:LOCK_OWNER;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;helperSha256=(Get-FileHash -LiteralPath $helper).Hash;mode=$Mode;settings=$settings;cleared=$clear;inherited=$saved;arms=@();
 scope='Stationary camera altitude affects rain population/projection/CPU phases. No occupied helicopter, flight, GPU timestamp or complete performance attribution claim. Timings are last-update phases sampled once per simulation second; event counts accumulate until diagnostic reset.'}
$p=$null;$client=$null
function Health{
 Require (!$p.HasExited -and [DateTime]::UtcNow -lt $deadline) 'Owned game exited or arm exceeded bounded wall time.'
 $live=@(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue);Require ($live.Count -eq 1 -and $live[0].Id -eq $p.Id) 'Foreign process contention.'
}
function Send($command){Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath $rpc;$writer.WriteLine($line)
 do{$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath $rpc;$reply=$replyLine|ConvertFrom-Json}while($null -eq $reply.ok);Require $reply.ok $replyLine;return $reply
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Clock{$q=Send @{cmd='query';what='play_state'};Require ($q.has_player -and $q.player_active -and $q.player_local) 'Actual stock player absent.';return [long]$q.time_ms}
function Close-Owned{
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit.'
 $meta=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$path=Join-Path $armDir 'lifecycle.json';$meta|ConvertTo-Json|Set-Content -LiteralPath $path
 & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $path -LogPath $log;$arm.lifecycle=$meta
}
try{
 foreach($view in @('Ground','Altitude117','Altitude300')){
  Same-Pair $before (Pair);Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Ownership conflict.'
  foreach($key in $keys){Restore-Environment $key $null};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
  $armDir=Join-Path $campaign $view;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force $profile|Out-Null
  & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
  $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;').Replace('fpsCap=0;','fpsCap=60;')))
  $mission=Join-Path $armDir 'ordinary.noe';New-Item -ItemType Directory -Force $mission|Out-Null;Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
  $env:POSEIDON_USER_DIR=$profile;$arm=[ordered]@{view=$view;status='running';profileSha256=(Get-FileHash -LiteralPath $cfg).Hash;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;weather=@();rows=@()};$result.arms+=,$arm
  $log=Join-Path $armDir 'engine.log';$rpc=Join-Path $armDir 'harness.jsonl';$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
  $deadline=[DateTime]::UtcNow.AddSeconds(240);$p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError (Join-Path $armDir 'stderr.txt');$null=$p.Handle;$arm.pid=$p.Id
  $until=[DateTime]::UtcNow.AddSeconds(120);do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness startup timeout.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Stock mission not ready.'
  Exec 'player allowDamage false;setDate [1985,6,21,16,0];0 setFog 0;0 setOvercast 1;0 setRain 1;triSetBrightness 1;setAccTime 0'
  $null=Send @{cmd='dev_snow';action='disable'}
  $bed=Send @{cmd='dev_terrain_brush';action='state';x=4975;z=4675};$null=Number $bed.vertexHeight;$arm.nativeBed=$bed
  $world=Send @{cmd='dev_mud';action='sample';x=4975;z=4675};Require ($world.world.Replace('\','/').ToLowerInvariant() -match '(^|/)noe\.wrp$') 'Actual stock Nogova not mounted.';$arm.world=$world.world
  $height=if($view -ceq 'Ground'){[double]$bed.vertexHeight+2}elseif($view -ceq 'Altitude117'){117.58}else{300.0}
  Require ((Eval ('triFreeFlyPose "4975 4675 '+(Number $height)+' 0 -12"')) -ceq 'OK') 'Camera pose refused.';$arm.pose=@(4975,4675,$height,0,-12);$arm.heightAboveBed=$height-[double]$bed.vertexHeight
  Exec 'setAccTime 1';$start=Clock
  do{Start-Sleep -Seconds 1;Health}while((Clock)-$start -lt $WarmupSeconds*1000)
  $readyStart=Clock;$weather=Send @{cmd='weather_visibility'}
  while($weather.rain -lt .95 -or $weather.liquidRain -lt .95 -or $weather.particleDensity -lt .95){
   Require ((Clock)-$readyStart -lt 30000) 'Actual dense liquid weather did not settle within30extra simulation seconds.'
   Start-Sleep -Seconds 1;Health;$weather=Send @{cmd='weather_visibility'}
  }
  Rain-Comparable $weather;$arm.extraWeatherReadyMs=(Clock)-$readyStart;$arm.readyWeather=$weather
  $lines=@(Read-CompleteLogLines $log);$firstRows=@(Parse-RainRows $lines);$arm.traceRowsBefore=$firstRows.Count;$arm.startMs=Clock
  do{Start-Sleep -Seconds 2;Health;$weather=Send @{cmd='weather_visibility'};Rain-Comparable $weather;$arm.weather+=@{timeMs=(Clock);actual=$weather}}while((Clock)-$arm.startMs -lt $MeasureSeconds*1000)
  Exec 'setAccTime 0';$arm.endMs=Clock;Require ($arm.endMs-$arm.startMs -ge $MeasureSeconds*1000) 'Simulation did not advance through requested interval.'
  $allRows=@(Parse-RainRows @(Read-CompleteLogLines $log));$arm.rows=@($allRows|Select-Object -Skip $arm.traceRowsBefore);$arm.summary=Summary $arm.rows
  Require ($arm.summary.drops.max -gt 0 -and $arm.summary.drawn.max -gt 0) 'No live and rendered rain proved.'
  Close-Owned;Same-Pair $before (Pair);$client.Dispose();$client=$null;$p=$null;$arm.status='measured-camera-altitude-only'
 }
 $allRain=@($result.arms|ForEach-Object{$_.weather|ForEach-Object{[double]$_.actual.liquidRain}})
 $range=($allRain|Measure-Object -Maximum).Maximum - ($allRain|Measure-Object -Minimum).Minimum
 Require ($range -le .02) 'Actual liquid rain differs by >.02 across altitude arms; performance comparison rejected.';$result.actualLiquidRainRange=$range
 $result.status='measured-camera-altitude-population-and-sampled-cpu-phases'
}catch{$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=$_.InvocationInfo.PositionMessage}
finally{
 if($p -and !$p.HasExited -and $client){try{Close-Owned}catch{$result.cleanupError=$_.Exception.Message;$result.status='failed'}}
 if($client){$client.Dispose()};if($p -and !$p.HasExited){$result.status='failed';$result.forcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$null=$p.WaitForExit(10000)}
 foreach($key in $keys){Restore-Environment $key $saved[$key]};$result.after=Pair;$result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath (Join-Path $campaign 'result.json');Write-Output "Rain population record: $campaign"
}
Require ($result.status -ceq 'measured-camera-altitude-population-and-sampled-cpu-phases') 'Rain population campaign failed; inspect retained record.'
