<# Installed stock Jeep/truck wheel contacts on original Nogova soil; no synthetic stamps. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,
 [ValidateSet('Jeep','Truck5t')][string]$VehicleClass='Jeep',[switch]$Resistance,[switch]$DisableResistance)
$ErrorActionPreference='Stop'
if($Resistance -and $DisableResistance){throw 'Choose resistance on or off'}
$resistanceEnabled=!$DisableResistance
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$root=Split-Path $PSScriptRoot -Parent;$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $root ('build/jeep-mud-runtime/'+(Get-Date -Format yyyyMMdd-HHmmss));$null=New-Item -ItemType Directory -Path $out -Force
$receipt=@{installed=$stamp;passed=$false;samples=@();scope='One actual stock vehicle, native cultivated soil, real rain and AI driving; bounded rut/resistance smoke, no tyre-slip or general vehicle validation'};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;$line|Add-Content (Join-Path $out 'harness.jsonl');return $reply}
function Exec($code){$null=Send @{cmd='exec';code=$code}}
function Eval($code){$reply=Send @{cmd='eval';code=$code};return ($reply.result|ConvertFrom-Json -NoEnumerate)}
try{
 $env:POSEIDON_USER_DIR=Join-Path $out 'user';$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 Remove-Item Env:POSEIDON_MUD_TRUCK_PROTOTYPE -ErrorAction SilentlyContinue
 Remove-Item Env:POSEIDON_MUD_TRACTION_PROTOTYPE -ErrorAction SilentlyContinue
 if($DisableResistance){$env:POSEIDON_MUD_TRACTION_PROTOTYPE='0'}
 $env:POSEIDON_MUD_TRACE='1';$env:WGR_GRASS='0';$env:WGR_TEMPORAL='0';$env:POSEIDON_SNOWLINE='off'
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log';$stderr=Join-Path $out 'stderr.log';$mission=Join-Path $root 'tests/perf/missions/perf_mud.noe'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','800','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene deadline';Start-Sleep -Milliseconds 300}
 Exec ('setAccTime 0;player allowDamage false;player setPos [4970,4675,0];mudJeep="{0}" createVehicle [4975,4675,0];mudJeep setDir 0;mudJeep allowDamage false;mudGroup=createGroup west;"SoldierWB" createUnit [[4975,4675,0],mudGroup,"mudDriver=this"];mudDriver moveInDriver mudJeep;mudDriver allowDamage false;mudDriver setBehaviour "CARELESS";0 setFog 0;0 setRain 0;0 setOvercast 0' -f $VehicleClass)
 $source=Send @{cmd='dev_mud';action='sample';x=4975;z=4675};Require $source.sourceEligible 'Wrong actual soil';$receipt.source=$source
 Exec 'triFreeFlyPose "4972 4670 95 25 -50";setAccTime 1';Start-Sleep -Seconds 3;Exec 'setAccTime 0'
 $dry=Send @{cmd='dev_mud';action='state'};Require ($dry.chunks-eq0) 'Dry contacts deformed ground';$receipt.dry=$dry
 Exec '0 setOvercast 1;0 setRain 1;setAccTime 4';$deadline=[DateTime]::UtcNow.AddSeconds(90)
 do{Start-Sleep -Milliseconds 300;$wet=Send @{cmd='dev_mud';action='state'};Require ([DateTime]::UtcNow-lt$deadline) 'Natural rain wetting deadline'}while($wet.wetness-lt.85)
 Exec 'setAccTime 0';$receipt.wet=Send @{cmd='dev_mud';action='state'};$receipt.weather=Send @{cmd='weather_visibility'}
 $start=Eval 'getPosASL mudJeep';$receipt.start=$start
 Exec 'mudDriver doMove [4975,4710,0];setAccTime 1';$deadline=[DateTime]::UtcNow.AddSeconds(35)
 do{Start-Sleep -Milliseconds 300;$position=Eval 'getPosASL mudJeep';$receipt.samples+=@{position=$position;mud=(Send @{cmd='dev_mud';action='state'})};$distance=[Math]::Sqrt([Math]::Pow($position[0]-$start[0],2)+[Math]::Pow($position[1]-$start[1],2));Require ([DateTime]::UtcNow-lt$deadline) 'Jeep did not actually drive'}while($distance-lt8)
 Exec 'setAccTime 0';$receipt.finish=$position;$receipt.distance=$distance
 $trace=@(Select-String -LiteralPath $log -Pattern 'MUD_WHEEL' | ForEach-Object {$_.Line});Require ($trace.Count-gt8) 'Actual Jeep wheel mutation trace absent';$receipt.trace=$trace
 $min=0.0;$probes=@();foreach($line in $trace|Select-Object -Last 30){Require ($line-match 'MUD_WHEEL x=([\d.eE+-]+) z=([\d.eE+-]+)') 'Trace grammar';$sample=Send @{cmd='dev_mud';action='sample';x=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);z=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)};$min=[Math]::Min($min,$sample.offset);$probes+=,$sample}
 $limit=if($VehicleClass-eq'Truck5t'){.16001}else{.12001}
 Require ($min-lt-.015-and$min-ge-$limit) 'No bounded real wheel rut';$receipt.probes=$probes;$receipt.minimumOffset=$min
 $resistanceTrace=@(Select-String -LiteralPath $log -Pattern 'MUD_RESISTANCE' | ForEach-Object {$_.Line})
 if($resistanceEnabled){Require ($resistanceTrace.Count-gt0) 'Actual admitted-wheel resistance absent'}
 else{Require ($resistanceTrace.Count-eq0) 'Opt-out unexpectedly changed physics'}
 $receipt.vehicleClass=$VehicleClass;$receipt.resistance=[bool]$resistanceEnabled;$receipt.resistanceTrace=$resistanceTrace
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'jeep-ruts.png')}
 $near=$probes[$probes.Count-1];$culture=[Globalization.CultureInfo]::InvariantCulture
 $pose=(@(($near.x-2.5),($near.z-3.0),($near.surfaceY+2.8),40,-28)|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ' '
 Exec ('triFreeFlyPose "'+$pose+'"');Start-Sleep -Seconds 3
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'jeep-ruts-close.png')}
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Normal exit failed'
 Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Unbekannter Operator' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Pair changed';$receipt.passed=$true;Write-Host ('Evidence: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()};$receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
