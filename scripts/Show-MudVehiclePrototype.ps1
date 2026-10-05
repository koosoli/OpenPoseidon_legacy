[CmdletBinding()]
param([ValidateSet('Jeep','Truck5t')][string]$VehicleClass='Jeep',[switch]$Resistance,[switch]$DisableResistance,[switch]$BootRelief,[string]$ExpectedCommit='')
$ErrorActionPreference='Stop'
if($Resistance -and $DisableResistance){throw 'Choose resistance on or off'}
$resistanceEnabled=!$DisableResistance
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$root=Split-Path $PSScriptRoot -Parent
$mission=Join-Path $root 'tests/perf/missions/perf_mud.noe'
$out=Join-Path $root ('build/mud-vehicle-owner-'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Path $out
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:WGR_GRASS='0'
$env:POSEIDON_SNOWLINE='off'
$env:POSEIDON_SNOW_TEST_DEPTH='0'
Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
Remove-Item Env:POSEIDON_MUD_TRUCK_PROTOTYPE -ErrorAction SilentlyContinue
Remove-Item Env:POSEIDON_MUD_TRACTION_PROTOTYPE -ErrorAction SilentlyContinue
if($DisableResistance){$env:POSEIDON_MUD_TRACTION_PROTOTYPE='0'}
Remove-Item Env:POSEIDON_BOOT_RELIEF_PROTOTYPE -ErrorAction SilentlyContinue
if($BootRelief){$env:POSEIDON_BOOT_RELIEF_PROTOTYPE='1'}
$env:POSEIDON_MUD_TRACE='1'
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if($ExpectedCommit -and !$stamp.Contains($ExpectedCommit)){throw 'Installed build changed'}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$p=$null;$client=$null;$writer=$null;$ready=$false
function Send($command){
 $writer.WriteLine(($command|ConvertTo-Json -Compress))
 do {$line=$reader.ReadLine();if(!$line){throw 'Harness disconnected'};$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
 if(!$reply.ok){throw $line};return $reply
}
function Exec($code){$null=Send @{cmd='exec';code=$code}}
try{
 $argv=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','1800','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+(Join-Path $out 'engine.log')+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Normal -ArgumentList $argv -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(120)
 do{
  if($p.HasExited){throw 'Game startup exited'}
  $client=[Net.Sockets.TcpClient]::new()
  try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
  if(!$client){if([DateTime]::UtcNow -gt $deadline){throw 'Startup deadline'};Start-Sleep -Milliseconds 300}
 }while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 do{
  $scene=Send @{cmd='eval';code='triSceneReady'}
  if([DateTime]::UtcNow -gt $deadline){throw 'Scene deadline'}
  if(([string]$scene.result).Trim('"') -ne 'OK'){Start-Sleep -Milliseconds 300}
 }while(([string]$scene.result).Trim('"') -ne 'OK')
 Exec ('setAccTime 0;player allowDamage false;player setPos [4970,4675,0];mudJeep="{0}" createVehicle [4975,4675,0];mudJeep setDir 0;mudJeep allowDamage false;0 setFog 0;0 setOvercast 0;0 setRain 0;setAccTime 1' -f $VehicleClass)
 Start-Sleep -Seconds 3
 Exec '0 setOvercast 1;0 setRain 1;setAccTime 4'
 $probe=Send @{cmd='dev_mud';action='sample';x=4975;z=4675}
 if(!$probe.sourceEligible){throw 'Wrong mud soil'}
 $deadline=[DateTime]::UtcNow.AddSeconds(75)
 do{
  Start-Sleep -Milliseconds 400
  $mud=Send @{cmd='dev_mud';action='state'}
  $mud|ConvertTo-Json -Compress|Set-Content (Join-Path $out 'wetting.json')
  Send @{cmd='weather_visibility'}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'weather.json')
  if([DateTime]::UtcNow -gt $deadline){throw 'Rain wetting deadline'}
 }while($mud.wetness -lt .65)
 Exec 'setAccTime 1;player moveInDriver mudJeep;hint "Ready: the field is wet. Drive forward and reverse over your tracks, then get out to inspect the ruts. Third-person view: Numpad Enter."'
 $driver=Send @{cmd='eval';code='vehicle player == mudJeep'}
 if(([string]$driver.result).Trim('"') -ne 'true'){throw 'Player not in Jeep'}
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'ready.png')}
 @{status='ready';vehicleClass=$VehicleClass;resistance=[bool]$resistanceEnabled;pid=$p.Id;installed=$stamp;mission=$mission;wetness=$mud.wetness;driver=$driver.result;output=$out}|ConvertTo-Json|Set-Content (Join-Path $root 'build/mud-vehicle-owner-ready.json')
 $ready=$true
 Write-Host "Mud vehicle demo ready: PID $($p.Id), wetness $($mud.wetness), $out"
 $client.Dispose();$client=$null
 $p.WaitForExit()
}finally{
 if(!$ready -and $p -and !$p.HasExited){
  if($writer){try{$null=Send @{cmd='exit'}}catch{}}
  if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}
 }
 if($client){$client.Dispose()}
}
