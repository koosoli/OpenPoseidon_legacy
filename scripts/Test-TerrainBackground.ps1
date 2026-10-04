# Installed original Everon: same-build legacy cutoff / background coverage comparison.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='everon-terrain-background',[switch]$MapOnly,[switch]$OnlyMapExtent)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $game 'DEPLOYED-FROM.txt') -Raw;Require ($stamp-match [regex]::Escape($ExpectedCommit)) 'Installed source differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{@{name=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $game $_)).Hash}})}
}
Require ([bool]$env:LOCK_OWNER) 'Game lock required.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserving existing game.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/terrain-background/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss));New-Item -ItemType Directory -Force $taskOut|Out-Null
$keys=@('POSEIDON_USER_DIR','POSEIDON_AUTOMATIC_RAGDOLL','WGR_TERRAIN_BACKGROUND','WGR_TERRAIN_EXTENT','WGR_WATER_EXTENT','WGR_GRASS','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_TONEMAP','WGR_LAYERED_FOG','WGR_FOG_LAYER0','WGR_FOG_LAYER1','WGR_FOG_TERRAIN','WGR_FOG_PATCH','WGR_CLOUD_COVERAGE','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLE_WETNESS')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;source=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;arms=@();scope='Producer terrain selection and bounded original Everon imagery. GPU/CPU timers are asynchronous published samples without screenshot frame identity; no inclusive timer summation or statistical performance acceptance.'}
$p=$null;$client=$null;$reference=$null
function Send($command,[bool]$allowError=$false){
 $writer.WriteLine(($command|ConvertTo-Json -Depth 8 -Compress));do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null-eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $armOut 'harness-replies.jsonl');Require ($allowError-or $reply.ok) $line;return $reply
}
function Eval([string]$code){$v=([string](Send @{cmd='eval';code=$code}).result).Trim();if($v.StartsWith('"')){return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json -InputObject $v -NoEnumerate}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
try{
 $arms=@(@{name='legacy';background='0';extent='3'},@{name='background';background='1';extent='3'});if($MapOnly){$arms+=@{name='map-only';background='1';extent='1'}}
 if($OnlyMapExtent){$arms=@(@{name='map-only';background='1';extent='1'})}
 foreach($arm in $arms){
  Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Foreign game appeared.'
  $armOut=Join-Path $taskOut $arm.name;$armProfile=Join-Path $armOut 'user';New-Item -ItemType Directory -Force $armProfile|Out-Null
  [IO.File]::WriteAllText((Join-Path $armProfile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
  foreach($key in $keys){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
  $settings=@{POSEIDON_USER_DIR=$armProfile;POSEIDON_AUTOMATIC_RAGDOLL='0';WGR_TERRAIN_BACKGROUND=$arm.background;WGR_TERRAIN_EXTENT=$arm.extent;WGR_GRASS='0';WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='0.6';WGR_TONEMAP='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
  foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
  $log=Join-Path $armOut 'engine.log';$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden';$argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','3500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
  $p=Start-Process -FilePath (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -PassThru
  $until=[DateTime]::UtcNow.AddSeconds(120)
  do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  while((Eval 'triSceneReady')-cne 'OK'){Require ([DateTime]::UtcNow-lt $until) 'Scene deadline.';Start-Sleep -Milliseconds 250}
  Exec 'player allowDamage false;0 setRain 0;0 setOvercast 0;setDate [1985,6,21,12,0]'
  $nearHeight=[Math]::Max(0,[double](Eval 'triTerrainHeight [9698,1622]'))+5
  $views=@(@{name='upper';pose=@(6000,1200,4000,0,-.1816,.9834);vd=3500;fog=0},@{name='moved';pose=@(6000,1200,4500,0,-.1816,.9834);vd=3500;fog=0},@{name='high';pose=@(6000,5000,4000,0,-.65,.76);vd=3500;fog=0},@{name='wide';pose=@(6000,1200,4000,0,-.1816,.9834);vd=50000;fog=0},@{name='near';pose=@(9698,$nearHeight,1622,0,-.05,1);vd=3500;fog=0},@{name='fogged';pose=@(6000,1200,4000,0,-.1816,.9834);vd=3500;fog=.45})
  $receipt=@{name=$arm.name;settings=$settings;views=@()};$result.arms+=,$receipt
  foreach($view in $views){
   Require ((Eval ('triSetViewDistance '+$view.vd))-like 'OK:*') 'View distance setter failed.'
   Exec ('0 setFog '+$view.fog.ToString([Globalization.CultureInfo]::InvariantCulture)+';setAccTime 1');Start-Sleep -Seconds 2
   $until=[DateTime]::UtcNow.AddSeconds(20)
   do{$weather=Send @{cmd='weather_visibility'};Require ([DateTime]::UtcNow-lt $until) 'Weather deadline.';if([Math]::Abs([double]$weather.fog-$view.fog)-gt .001){Start-Sleep -Milliseconds 250}}while([Math]::Abs([double]$weather.fog-$view.fog)-gt .001)
   Require ([double]$weather.rain-lt .001-and [double]$weather.overcast-lt .001) 'Dry weather changed.'
   Exec 'setAccTime 0';$poseText=($view.pose|ForEach-Object{([double]$_).ToString('R',[Globalization.CultureInfo]::InvariantCulture)})-join ','
   Require ((Eval ('triSetView ['+$poseText+']'))-ceq 'OK') 'Camera refused.'
   $start=Send @{cmd='query';what='play_state'};$until=[DateTime]::UtcNow.AddSeconds(90)
   do{$now=Send @{cmd='query';what='play_state'};Require ([DateTime]::UtcNow-lt $until) 'Rendered warmup deadline.';if([double]$now.frame-[double]$start.frame-lt 256){Start-Sleep -Milliseconds 250}}while([double]$now.frame-[double]$start.frame-lt 256)
   $source=Send @{cmd='dev_cave_editor';action='state';x=$view.pose[0];z=$view.pose[2];y=1000}
   Require ($source.worldName.Replace('\','/').ToLowerInvariant()-match '(^|/)eden\.wrp$'-and $source.count-eq 0) 'Not untouched original Everon.'
   if(!$reference){$reference=$source}else{Require ($source.heightRevision-eq $reference.heightRevision-and $source.worldName-ceq $reference.worldName-and $source.grid-eq $reference.grid) 'Source changed.'}
   $coverage=Send @{cmd='terrain_coverage'} ($arm.background-eq '0')
   if($coverage.ok){
    Require ($coverage.ready-and $coverage.background-eq ($arm.background-eq '1')-and !$coverage.pagedMaterials) 'Coverage/material control differs.'
    foreach($axis in 0..2){Require ([Math]::Abs([double]$coverage.camera[$axis]-[double]$view.pose[$axis])-lt .03) 'Camera witness differs.'}
    Require ($coverage.heightRevision-eq $source.heightRevision-and $coverage.terrainGrid-gt 0-and $coverage.patches-gt 0) 'Coverage source unavailable.'
    # At 5000m the altitude-expanded legacy rectangle already covers the entire
    # authored map. Extended seabed still adds background; map-only need not.
    if($arm.background-eq '1'-and ($view.name-in @('upper','moved','fogged')-or ($view.name-eq 'high'-and $arm.extent-eq '3'))){Require ($coverage.outsideLegacyRect-gt 0) 'No distant coverage beyond old cutoff.'}
   }else{Require ($arm.background-eq '0') 'Default coverage unavailable.'}
   $weatherBefore=Send @{cmd='weather_visibility'};$performanceBefore=Send @{cmd='frame_performance'}
   $path=Join-Path $armOut ($view.name+'.png');$null=Send @{cmd='screenshot';path=$path}
   $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow-lt $until) 'Screenshot write deadline.';Start-Sleep -Milliseconds 100}
   Require ((Get-Item -LiteralPath $path).Length-gt 4096) 'Empty screenshot.'
   $performance=Send @{cmd='frame_performance'};$weatherAfter=Send @{cmd='weather_visibility'}
   Require ([Math]::Abs([double]$weatherAfter.fog-$view.fog)-lt .001-and $weatherAfter.terrainRange-eq $weatherBefore.terrainRange-and $weatherAfter.objectRange-eq $weatherBefore.objectRange) 'Weather/range changed during capture.'
   foreach($field in @('renderWidth','renderHeight','outputWidth','outputHeight','msaaSamples','activeUpscaler')){Require ($performance.$field-eq $performanceBefore.$field) 'Render configuration changed.'}
   $receipt.views+=@{name=$view.name;pose=$view.pose;requestedViewDistance=$view.vd;renderedWarmup=([double]$now.frame-[double]$start.frame);source=$source;coverage=$coverage;weatherBefore=$weatherBefore;weatherAfter=$weatherAfter;performance=$performance;png=$path;pngSha256=(Get-FileHash -LiteralPath $path).Hash}
  }
  $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000)-and $p.ExitCode-eq 0) 'Normal exit failed.'
  Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Runtime renderer failure.'
  $receipt.exitCode=$p.ExitCode;$client.Dispose();$client=$null;$p=$null
 }
 Require ((Pair|ConvertTo-Json -Depth 6 -Compress)-ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed.'
 $result.passed=$true;Write-Output "Terrain background installed controls PASS; image judgement pending: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p-and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(15000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null-eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
