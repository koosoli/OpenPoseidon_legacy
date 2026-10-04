# Installed original Everon. Root runtime owner invokes through with-game-lock.sh.
[CmdletBinding()]
param(
 [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='layered-asl-fog',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [double]$X=6532,[double]$Z=6466,[ValidateRange(0.05,0.8)][double]$WeatherFog=0.45
)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($v){$n=[double]::Parse([string]$v,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite receipt.';return $n}
function Fmt($v){return (Num $v).ToString('R',$culture)}
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete string.';return $value.Substring(1,$value.Length-2).Replace('""','"')};return ConvertFrom-Json $value -NoEnumerate}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp -match [regex]::Escape($ExpectedCommit)) 'Installed commit differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$file=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length}})}
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/layered-fog/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'));New-Item -ItemType Directory -Force $taskOut|Out-Null
$keys=@('POSEIDON_USER_DIR','WGR_LAYERED_FOG','WGR_LAYERED_FOG_TRACE','WGR_FOG_LAYER0','WGR_FOG_LAYER1','WGR_WEATHER_COVER','WGR_WEATHER_COVER_FAR','WGR_GRASS','WGR_TEMPORAL','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_TEST_RAIN','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_CLOUD_COVERAGE')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;arms=@();scope='Actual original Everon and authoritative weather, same XYZ camera requests, unchanged native source/ranges at equal weather. Encoded trace is CPU command readiness, not GPU completion or visual acceptance. Default remains OFF. No tactical visibility changes, interior fog, full coupled atmosphere, persistent UI setting or timing acceptance claimed.'}
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
  foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$null,'Process')}
  $settings=@{POSEIDON_USER_DIR=$profile;WGR_LAYERED_FOG=$arm.on;WGR_LAYERED_FOG_TRACE='1';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_FAR='1';WGR_GRASS='0';WGR_TEMPORAL='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
  if($layerArgs){$settings.WGR_FOG_LAYER0=$layerArgs[0];$settings.WGR_FOG_LAYER1=$layerArgs[1]}
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
  $until=[DateTime]::UtcNow.AddSeconds(30)
  do{$weather=Send @{cmd='weather_visibility'};if([Math]::Abs((Num $weather.fog)-$arm.fog) -gt .001 -or (Num $weather.overcast) -gt .001){Require ([DateTime]::UtcNow -lt $until) 'Actual weather did not settle.';Start-Sleep -Milliseconds 300}else{break}}while($true)
  Exec 'setAccTime 0'
  $source=Send @{cmd='dev_cave_editor';action='state';x=$X;z=$Z;y=1000}
  Require ($source.worldName.Replace('\','/').ToLowerInvariant() -match '(^|/)eden\.wrp$' -and $source.count -eq 0) 'Not untouched original Everon.'
  $actualGround=Num (Eval ('triTerrainHeight ['+(Fmt $X)+','+(Fmt $Z)+']'))
  if($null -eq $ground){
   $ground=$actualGround
   $lower=(@(($ground-3),($ground+18),3,.008)|ForEach-Object{Fmt $_}) -join ','
   $upper=(@(($ground+35),($ground+50),3,.004)|ForEach-Object{Fmt $_}) -join ','
   $layerArgs=@($lower,$upper)
   $baselineSource=$source;$baselineWeather=$weather;$result.profile=@{siteXZ=@($X,$Z);nativeY=$ground;layers=$layerArgs;policy='Frozen world-ASL slabs selected once from actual baseline height. They do not follow terrain. OFF scout used built-in profiles; disabled optics are independent of profile.'}
  }else{Require ([Math]::Abs($actualGround-$ground) -lt .0001 -and $source.worldName -ceq $baselineSource.worldName -and $source.heightRevision -eq $baselineSource.heightRevision -and $source.grid -eq $baselineSource.grid) 'Native source changed between arms.'}
  if($arm.fog -gt 0){foreach($range in @('terrainRange','baseRange','objectRange','tacticalVisibility')){Require ([Math]::Abs((Num $weather.$range)-(Num $baselineWeather.$range)) -lt .001) ('Existing '+$range+' changed at identical weather.')}}
  $receipt=@{name=$arm.name;settings=$settings;weather=$weather;source=$source;nativeY=$actualGround;views=@()};$result.arms+=,$receipt
  foreach($view in @(@{name='ground';y=($ground+2)},@{name='between';y=($ground+26)},@{name='above';y=($ground+75)})){
   $pose=@($X,$view.y,$Z,-.65,-.07,-.76);Require ((Eval ('triSetView ['+(($pose|ForEach-Object{Fmt $_}) -join ',')+']')) -ceq 'OK') 'Render-only camera refused.'
   Start-Sleep -Seconds 4;$path=Join-Path $armOut ($view.name+'.png');$null=Send @{cmd='screenshot';path=$path}
   $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow -lt $until) 'Screenshot deadline.';Start-Sleep -Milliseconds 150}
   Require ((Get-Item -LiteralPath $path).Length -gt 4096) 'Capture is empty.'
   $rows=@(Logs|Where-Object{$_ -match '\[wgr\] layered fog:.*camera=\['}|Select-Object -Last 8)
   $matching=@($rows|Where-Object{$_ -match 'weather=([-0-9.]+).*camera=\[([-0-9.eE+]+), ([-0-9.eE+]+), ([-0-9.eE+]+)' -and [Math]::Abs((Num $Matches[1])-$arm.fog) -lt .001 -and [Math]::Abs((Num $Matches[2])-$X) -lt .03 -and [Math]::Abs((Num $Matches[3])-$view.y) -lt .03 -and [Math]::Abs((Num $Matches[4])-$Z) -lt .03})
   Require ($matching.Count -gt 0) 'No actual encoded-camera/weather trace at captured view.'
   if($arm.on -eq '1' -and $arm.fog -gt 0){Require ([bool]($matching|Where-Object{$_ -match 'encoded=true'})) 'Current source/physical coverage never admitted fog.'}else{Require (![bool]($matching|Where-Object{$_ -match 'encoded=true'})) 'Disabled/zero-weather control encoded fog.'}
   $viewWeather=Send @{cmd='weather_visibility'}
   Require ([Math]::Abs((Num $viewWeather.fog)-$arm.fog) -lt .001 -and (Num $viewWeather.rain) -lt .001 -and (Num $viewWeather.overcast) -lt .001) 'Weather changed at capture.'
   $receipt.views+=@{name=$view.name;cameraXYZDirectionXYZ=$pose;path=$path;trace=$matching;weather=$viewWeather}

  }
  $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000) -and $p.ExitCode -eq 0) 'Normal game exit failed.';$receipt.exit=$p.ExitCode
  Require (!(Logs|Select-String -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION')) 'Renderer failure logged.'
  $client.Dispose();$client=$null;$p=$null
 }
 $after=Pair;Require (($after|ConvertTo-Json -Depth 6 -Compress) -ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed.'
 $result.passed=$true;Write-Host "Installed layered fog functional controls PASS; image judgement pending: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p -and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
