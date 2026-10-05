<# Installed main menu, native local content browser, and child restart. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[ValidateSet("None","Map","Vehicle")][string]$Preview="None",[string]$MapName="Stratis",[switch]$Fullscreen)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$root=Split-Path $PSScriptRoot -Parent
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $root ('build/local-browser/'+$Preview+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Path $out -Force
$receipt=@{installed=$stamp;passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json -AsHashtable}while($null-eq$reply.ok);Require $reply.ok $line;$line|Add-Content (Join-Path $out 'harness.jsonl');return $reply}
function Exec($code){$null=Send @{cmd='exec';code=$code};Start-Sleep -Milliseconds 250}
function Text($id){return ([string](Send @{cmd='eval';code=('triControlText '+$id)}).result).Trim('"')}
try{
 if($Preview -eq 'Map' -and $MapName -eq 'Everon'){
  # Verify browser defaults rather than accidentally inheriting a manual launch's opt-in.
  foreach($name in @('POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_MAX_MODELS','POSEIDON_REFORGER_NEAR_RADIUS','POSEIDON_REFORGER_MAX_OBJECTS')){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
 }
 $env:POSEIDON_CACHE_DIR=Join-Path $out 'cache';$null=New-Item -ItemType Directory -Path $env:POSEIDON_CACHE_DIR
 $env:POSEIDON_USER_DIR=Join-Path $out 'user';$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'),"preferredViewDistance=700;`ntextLanguage=""English"";`nvoiceLanguage=""English"";`n")
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log';$stdout=Join-Path $out 'stdout.log';$stderr=Join-Path $out 'stderr.log'
 $args=@('--render=wgpu','--dev','--width','1280','--height','720','--harness',"$port",'--lang','English','--log-file',('"'+$log+'"'))
 if($Fullscreen){$args+=@('--display-mode','borderless')}else{$args+=@('--window')}
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(180)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 do{$idd=([string](Send @{cmd='eval';code='triDisplay'}).result).Trim('"');Require ([DateTime]::UtcNow-lt$deadline) 'Menu deadline';Start-Sleep -Milliseconds 250}while([int]$idd-lt0)
 Start-Sleep -Seconds 8
 $receipt.logo=Text 1988;Require ($receipt.logo -match 'openposeidon_logo.paa') 'Logo control missing'
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'menu.png')}
 Exec 'triClick 62000'
 $until=[DateTime]::UtcNow.AddSeconds(45)
 do{$status=Text 62005;Require ([DateTime]::UtcNow -lt $until) ('Catalog deadline: '+$status);Start-Sleep -Milliseconds 400}while($status -notmatch 'maps found')
 $receipt.catalogStatus=$status;$count=[int]([string](Send @{cmd='eval';code='triListSize 62001'}).result)
 Require ($count -gt 5) 'Steam maps not found';$receipt.maps=@()
 foreach($i in 0..($count-1)){$receipt.maps+=([string](Send @{cmd='eval';code=('triListText [62001,'+$i+']')}).result).Trim('"')}
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'maps.png')}
 Exec 'triClick 62009';$count=[int]([string](Send @{cmd='eval';code='triListSize 62001'}).result);Require ($count -eq 6) 'Vehicle catalog differs'
 $receipt.vehicles=@();foreach($i in 0..5){$receipt.vehicles+=([string](Send @{cmd='eval';code=('triListText [62001,'+$i+']')}).result).Trim('"')}
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'vehicles.png')}
 if($Preview -ne 'None'){
  if($Preview -eq 'Map'){
   Exec 'triClick 62008';$index=-1;for($i=0;$i-lt$receipt.maps.Count;$i++){if($receipt.maps[$i]-match ('\| '+[regex]::Escape($MapName)+' \|')){$index=$i}}
   Require ($index-ge0) ('Map not found: '+$MapName);Exec ('triSelectList [62001,'+$index+']');$receipt.selected=$receipt.maps[$index]
  }else{Exec 'triSelectList [62001,0]';$receipt.selected=$receipt.vehicles[0]}
  $oldPid=$p.Id;$writer.WriteLine((@{cmd="exec";code='triClick 62004'}|ConvertTo-Json -Compress))
  Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Browser parent normal exit failed'
  $client.Dispose();$client=$null;$writer=$null
  $owned=Get-CimInstance Win32_Process -Filter "Name='OpenPoseidon.exe'"|Where-Object {$_.CommandLine -match ('--wait-for-parent\s+'+$oldPid+'(?:\s|$)')}
  Require (@($owned).Count-eq1) 'Browser child identity differs';$p=Get-Process -Id $owned.ProcessId;$null=$p.Handle;$receipt.childPid=$p.Id;$receipt.childCommand=$owned.CommandLine
  $expectedMode=if($Fullscreen){'borderless'}else{'windowed'};Require ($owned.CommandLine -match ('--display-mode\s+'+$expectedMode)) 'Live display mode was not preserved'
  $deadline=[DateTime]::UtcNow.AddSeconds(180)
  do{$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require (!$p.HasExited-and[DateTime]::UtcNow-lt$deadline) 'Preview connection deadline';Start-Sleep -Milliseconds 300}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=120000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Preview ready deadline';Start-Sleep -Milliseconds 300}
  Start-Sleep -Seconds 5;$receipt.preview=Send @{cmd='diag_inspect';unit='player'}
  if($Preview-eq'Vehicle'){$receipt.near=Send @{cmd='diag_near';pos=@(6532,6476);r=35};Require (@($receipt.near.objects|Where-Object cls -eq 'CWR_T72_A1').Count-gt0) 'Bridge vehicle not created';Exec 'testTank=nearestObject [[6532,6476,0],"CWR_T72_A1"];player moveInDriver testTank';Start-Sleep -Seconds 1;$receipt.tankBefore=Send @{cmd='diag_inspect';unit='testTank'};$null=Send @{cmd='key';sc=26;hold=$true};Start-Sleep -Seconds 3;$null=Send @{cmd='key_up';sc=26};$receipt.tankAfter=Send @{cmd='diag_inspect';unit='testTank'}}
  else{
   $position=$receipt.preview.pos
   $receipt.world=Send @{cmd='dev_cave_editor';action='state';x=$position[0];y=$position[2];z=$position[1]}
   Require ($receipt.world.terrainY -gt 0) 'Map preview did not start over land'
  }
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'preview.png')}
 }
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Normal exit failed'
 $logs=@($log,$stderr);$childLog=Join-Path $env:POSEIDON_CACHE_DIR 'local-map-browser.log';if(Test-Path $childLog){$logs+=$childLog}
 if($Preview -eq 'Map' -and $receipt.selected -match '^Arma Reforger'){
  $built=Select-String -Path $childLog -Pattern 'Enfusion native load: built ([1-9][0-9]*) of ([0-9]+) meshes .* placed ([1-9][0-9]*) objects'|Select-Object -Last 1
  if($built){$receipt.objectImport=$built.Line.Trim()}else{
   # Native streaming defers mesh/placement creation until the viewer is ready.
   $streamed=Select-String -Path $childLog -Pattern 'Enfusion native load: STREAMING -- ([1-9][0-9]*) placements'|Select-Object -Last 1
   $roads=Select-String -Path $childLog -Pattern 'Enfusion native load: ([0-9]+) roads built'|Select-Object -Last 1
   $resident=Select-String -Path $childLog -Pattern 'Retained admission census: ([0-9]+) objects offered, ([0-9]+) admitted'|Select-Object -Last 1
   Require ($streamed-and$roads-and$resident) 'Reforger streaming evidence missing'
   Require ([int]$resident.Matches[0].Groups[2].Value -gt ([int]$roads.Matches[0].Groups[1].Value+2)) 'Reforger residency contains only roads'
   $receipt.objectImport=$streamed.Line.Trim();$receipt.objectResidency=$resident.Line.Trim()
  }
 }
 Require (!(Select-String -Path $logs -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Cannot load --test-world' -Quiet)) 'Runtime or map-loading error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed pair changed'
 $receipt.passed=$true;Write-Host ('Evidence: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()};$receipt|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'result.json')
}
