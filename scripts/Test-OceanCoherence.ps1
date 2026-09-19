param(
 [switch]$CurlingBaseline, [switch]$Curling, [switch]$PerfOnly, [switch]$Menu, [switch]$LegacyShore, [switch]$Matrix, [switch]$NoFft, [ValidateRange(0,35)][double]$WindSpeed=8, [switch]$LiveClip, [switch]$LowQuality, [switch]$Surf, [ValidateRange(0,5)][int]$Preset=1, [string]$Label='current', [string]$BinaryDir='', [switch]$Quick, [switch]$Review,
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir)
 if ($BinaryDir) {$argv+=@('-BinaryDir',$BinaryDir)}
 $argv+=@('-Preset',"$Preset")
 if ($Surf) {$argv+='-Surf'}
 if ($Curling) {$argv+='-Curling'}
 if ($CurlingBaseline) {$argv+='-CurlingBaseline'}
 if ($LegacyShore) {$argv+='-LegacyShore'}
 if ($Menu) {$argv+='-Menu'}
 if ($PerfOnly) {$argv+='-PerfOnly'}
 $argv+=@('-WindSpeed',$WindSpeed.ToString([Globalization.CultureInfo]::InvariantCulture))
 if ($Matrix) {$argv+='-Matrix'}
 if ($NoFft) {$argv+='-NoFft'}
 if ($LiveClip) {$argv+='-LiveClip'}
 if ($LowQuality) {$argv+='-LowQuality'}
 if ($Quick) {$argv+='-Quick'}
 if ($Review) {$argv+='-Review'}
 $env:LOCK_OWNER='ocean coherence capture'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Ocean capture exited $LASTEXITCODE"}}
 finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
if (!$BinaryDir) {$BinaryDir=Join-Path $root 'dist/x64-win-rwdi'}
$out=Join-Path $root ('build/ocean-runs/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
$profileDir=Join-Path $env:POSEIDON_USER_DIR 'water-look'
New-Item -ItemType Directory -Force $profileDir | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$qualityFlag=if ($LowQuality) {'1'} else {'0'}
$referenceScale=if ($Preset -ge 3) {'1.0'} else {'0.65'}
$coupling=if ($Preset -ge 3) {'0'} else {'1'}
# Existing owner scale, controlled material. No stored developer overrides leak in.
[IO.File]::WriteAllText((Join-Path $profileDir 'noe_noe_wrp.cfg'),"v 5`nwave 1 1 1 $referenceScale`nquality $qualityFlag 1 512 $Preset`nsea $coupling 0.1 1`n")
if ($LegacyShore) {$env:WGR_WATER_SURF_PILOT='0'} elseif ($Surf) {$env:WGR_WATER_SURF_PILOT='1'} else {Remove-Item Env:WGR_WATER_SURF_PILOT -ErrorAction SilentlyContinue}
$env:POSEIDON_WIND_OVERRIDE=$WindSpeed.ToString([Globalization.CultureInfo]::InvariantCulture)+' 90 0'
if ($NoFft) {$env:WGR_WATER_FFT='0'} else {$env:WGR_WATER_FFT='1'}
if ($Menu) {
 # Copy saved water settings into the private profile; never edit the owner's files.
 $ownerWater=Join-Path $env:APPDATA 'CWR/water-look'
 if (Test-Path $ownerWater) {Get-ChildItem -LiteralPath $ownerWater -Filter '*.cfg' | Copy-Item -Destination $profileDir}
 Remove-Item Env:POSEIDON_WIND_OVERRIDE -ErrorAction SilentlyContinue
}
if ($Curling) {$env:WGR_WATER_CURLING_BREAKER='1'} else {Remove-Item Env:WGR_WATER_CURLING_BREAKER -ErrorAction SilentlyContinue}
$env:WGR_OCEAN_PROFILE='1'
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.5'
$mission=Join-Path $root 'tests/perf/missions/ocean_shore.noe'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$log=Join-Path $out 'engine.log'
$arguments=@('-C',('"'+$GameDir+'"'),'--render=wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
if ($Menu) {$arguments=@('-C',('"'+$GameDir+'"'),'--render=wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--log-file',('"'+$log+'"'))}
if ($Review) {$arguments=$arguments | ForEach-Object {if ($_ -eq '--no-dev') {'--dev'} else {$_}}}
@{head=(& git -C $root rev-parse HEAD);arguments=$arguments;hashes=@(Get-FileHash (Join-Path $BinaryDir 'OpenPoseidon.exe'),(Join-Path $BinaryDir 'wgpu_renderer.dll'),(Join-Path $mission 'mission.sqm'));settings=@(Get-ChildItem Env: | Where-Object Name -Match '^(WGR|POSEIDON)' | Select-Object Name,Value);deployment=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'))} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
$sourcePatch=Join-Path $out 'source.patch'
& git -C $root diff --binary --output=$sourcePatch HEAD
$process=$null;$client=$null
try {
 $windowStyle=if ($Review) {'Normal'} else {'Hidden'}
 $process=Start-Process -FilePath (Join-Path $BinaryDir 'OpenPoseidon.exe') -ArgumentList $arguments -WorkingDirectory $BinaryDir -PassThru -WindowStyle $windowStyle
 $null=$process.Handle
 $client=[Net.Sockets.TcpClient]::new(); $deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {
  try {$client.Connect('127.0.0.1',$port)} catch {if ($process.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw}; Start-Sleep -Milliseconds 250}
 }
 $stream=$client.GetStream();$stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Harness closed'};$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl')
  $line | Add-Content (Join-Path $out 'harness.jsonl')
  if (!$reply.ok) {throw $line};return $reply
 }
 Start-Sleep -Seconds 12
 if ($Menu) {
  $null=Send @{cmd='water_probe'}
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'menu.png')}
  for ($i=0;$i -lt 24;$i++) {
   Start-Sleep -Milliseconds 250
   $null=Send @{cmd='screenshot';path=(Join-Path $out ('menu-{0:d3}.png' -f $i))}
  }
  $null=Send @{cmd='exit'}
  if (!$process.WaitForExit(15000) -or $process.ExitCode) {throw 'Menu test failed to exit cleanly'}
  if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error in menu log'}
  Write-Output "Ocean menu evidence: $out"
  return
 }
 $null=Send @{cmd='exec';code='oceanCamera="camera" camCreate [7900,4500,10]; oceanCamera cameraEffect ["internal","back"]; oceanCamera camSetTarget [7900,4600,0]; oceanCamera camSetFov 0.7; oceanCamera camCommit 0; showCinemaBorder false; hint ""'}
 $null=Send @{cmd='water_probe'}
 $bathymetry=Send @{cmd='water_bathymetry';x=7900;z=4550}
 if ($Preset -ge 3 -or $Surf -or $Curling -or $CurlingBaseline) {$null=Send @{cmd='exec';code='oceanCamera camSetPos [7875,4582,8]; oceanCamera camSetTarget [7828,4582,0]; oceanCamera camCommit 0'}}
 if ($Curling -or $CurlingBaseline) {
  # The mission tide is several metres above datum. Frame the actual waterline,
  # not the zero-height contour, otherwise this camera looks behind the break.
  Start-Sleep -Milliseconds 250
  $heightProbe=Send @{cmd='eval';code='getPosASL oceanCamera'}
  # The calibrated camera above was placed eight metres above this open water.
  $seaLevel=($heightProbe.result | ConvertFrom-Json)[2]-8.0
  $row=@($bathymetry.samples | Where-Object {[Math]::Abs($_[1]-4582.0) -lt 0.1})
  $shoreX=$null
  for ($i=0;$i -lt $row.Count-1;$i++) {
   $a=$row[$i];$b=$row[$i+1]
   if ($a[2] -ge $seaLevel -and $b[2] -lt $seaLevel) {
    $shoreX=$a[0]+($b[0]-$a[0])*($a[2]-$seaLevel)/($a[2]-$b[2]);break
   }
  }
  if ($null -eq $shoreX) {throw 'No waterline found for curling fixture camera'}
  $centre=($shoreX+12.054).ToString('F3',[Globalization.CultureInfo]::InvariantCulture)
  $camera=($shoreX+18.054).ToString('F3',[Globalization.CultureInfo]::InvariantCulture)
  $null=Send @{cmd='exec';code="oceanCamera camSetPos [$camera,4558,1.35]; oceanCamera camSetTarget [$centre,4582,1.35]; oceanCamera camSetFov 0.7; oceanCamera camCommit 0"}
 }
 if ($Review) {
  Write-Host 'Live ocean review. Use the Water tab to compare scales. Close the game to finish; no automatic timeout.'
  while (!$process.HasExited) {Start-Sleep -Milliseconds 250}
  return
 }
 # Undisturbed live interval: use only this interval's timers for cost comparisons.
 $null=Send @{cmd='exec';code='logInfo "OCEAN_PERF_BEGIN"; triPerfReset 0'}
 Start-Sleep -Seconds 12
 $null=Send @{cmd='eval';code='triPerfStats 0'}
 if ($Curling -or $CurlingBaseline) {
  $null=Send @{cmd='eval';code='getPosASL oceanCamera'}
  $null=Send @{cmd='eval';code='getDir oceanCamera'}
 }
 $null=Send @{cmd='exec';code='logInfo "OCEAN_PERF_END"'}
 if ($PerfOnly) {
  $null=Send @{cmd='exit'}
  if (!$process.WaitForExit(15000) -or $process.ExitCode) {throw 'Performance test failed to exit cleanly'}
  if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error in performance log'}
  Write-Output "Ocean performance evidence (no screenshots): $out"
  return
 }
 if ($Curling) {
  foreach ($stage in @(@{name='approach';time=2.0},@{name='barrel';time=5.25},@{name='collapse';time=6.4},@{name='wash';time=7.7})) {
   $null=Send @{cmd='water_probe';freezeTime=$true;fixedTime=$stage.time}
   Start-Sleep -Milliseconds 250
   $null=Send @{cmd='screenshot';path=(Join-Path $out ($stage.name+'.png'))}
  }
  $null=Send @{cmd='exit'}
  if (!$process.WaitForExit(15000) -or $process.ExitCode) {throw 'Curl test failed to exit cleanly'}
  if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error in curl log'}
  Write-Output "Curling breaker evidence: $out"
  return
 }
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'live-shore.png')}
 if ($LiveClip) {
  $clock=[Diagnostics.Stopwatch]::StartNew();$samples=@()
  for ($i=0;$i -lt 192;$i++) {
   $remaining=$i*125-$clock.ElapsedMilliseconds
   if ($remaining -gt 0) {Start-Sleep -Milliseconds $remaining}
   $file=('live-{0:d3}.png' -f $i)
   $samples+=@{file=$file;elapsed=$clock.Elapsed.TotalSeconds}
   $null=Send @{cmd='screenshot';path=(Join-Path $out $file)}
  }
  $samples | ConvertTo-Json | Set-Content (Join-Path $out 'live-samples.json')
 }
 $null=Send @{cmd='water_probe';freezeTime=$true;freezeFoam=$true;fixedTime=40.0;shoreGain=0.0}
 foreach ($scale in @(0.5,1.0,2.0)) {
  $null=Send @{cmd='water_probe';scale=$scale}
  Start-Sleep -Milliseconds 500
  $null=Send @{cmd='screenshot';path=(Join-Path $out ('scale-'+$scale.ToString([Globalization.CultureInfo]::InvariantCulture)+'.png'))}
 }
 if (!$Quick) {
  $null=Send @{cmd='water_probe';scale=[double]$referenceScale;shoreGain=0.1}
  foreach ($view in @('shore','elevated')) {
   if ($view -eq 'elevated' -and ($Preset -ge 3 -or $Surf)) {$null=Send @{cmd='exec';code='oceanCamera camSetPos [7880,4582,24]; oceanCamera camSetTarget [7830,4582,0]; oceanCamera camCommit 0'}}
   elseif ($view -eq 'elevated') {$null=Send @{cmd='exec';code='oceanCamera camSetPos [7900,4500,35]; oceanCamera camSetTarget [7900,4550,0]; oceanCamera camCommit 0'}}
   $samples=@()
   for ($i=0;$i -lt 96;$i++) {
    $time=40.0+$i/8.0
    $null=Send @{cmd='water_probe';fixedTime=$time}
    Start-Sleep -Milliseconds 75
    $file=('{0}-{1:d3}.png' -f $view,$i)
    $null=Send @{cmd='screenshot';path=(Join-Path $out $file)}
    $samples+=@{file=$file;time=$time}
   }
   $samples | ConvertTo-Json | Set-Content (Join-Path $out ($view+'-samples.json'))
  }
 }
 if ($Matrix) {
  $null=Send @{cmd='water_probe';freezeTime=$false;freezeFoam=$false;scale=1.0}
  foreach ($view in @(
   @{name='moving';code='oceanCamera camSetPos [7865,4600,6]; oceanCamera camSetTarget [7828,4582,0]; oceanCamera camCommit 1'},
   @{name='turn-fov';code='oceanCamera camSetTarget [7950,4582,0]; oceanCamera camSetFov 0.45; oceanCamera camCommit 0'},
   @{name='teleport';code='oceanCamera camSetPos [9200,4500,20]; oceanCamera camSetTarget [9500,4500,0]; oceanCamera camCommit 0'},
   @{name='underwater';code='oceanCamera camSetPos [7875,4582,-2]; oceanCamera camSetTarget [7828,4582,-2]; oceanCamera camSetFov 0.7; oceanCamera camCommit 0'},
   @{name='return';code='oceanCamera camSetPos [7875,4582,8]; oceanCamera camSetTarget [7828,4582,0]; oceanCamera camCommit 0'}
  )) {
   $null=Send @{cmd='exec';code=$view.code}
   Start-Sleep -Seconds 2
   $null=Send @{cmd='screenshot';path=(Join-Path $out ($view.name+'.png'))}
  }
 }
 $null=Send @{cmd='exit'}
 if (!$process.WaitForExit(15000)) {throw 'Game exit timed out'}
 if ($process.ExitCode) {throw "Game exited $($process.ExitCode)"}
 if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error in log'}
 Write-Output "Ocean evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($process -and !$process.HasExited) {Stop-Process -Id $process.Id;$process.WaitForExit()}
}
