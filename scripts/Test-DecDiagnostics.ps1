[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{7,40}$')][string]$ExpectedCommit,
 [switch]$ExperimentalAi
)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Run through with-game-lock.sh.'}
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Existing owner game is preserved.'}
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskGame='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$taskStamp=Get-Content -LiteralPath (Join-Path $taskGame 'DEPLOYED-FROM.txt') -Raw
if($taskStamp -notmatch [regex]::Escape($ExpectedCommit)){throw 'Installed commit differs.'}
$taskFiles=@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{
 $file=Get-Item -LiteralPath (Join-Path $taskGame $_)
 @{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length;writtenUtc=$file.LastWriteTimeUtc.ToString('o')}
}
$taskOut=Join-Path $taskRoot ('build/dec-diagnostics/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$taskProfile=Join-Path $taskOut 'user';New-Item -ItemType Directory -Force $taskProfile | Out-Null
$env:POSEIDON_USER_DIR=$taskProfile
$env:POSEIDON_REFORGER_WORLD='worlds/eden';$env:POSEIDON_REFORGER_OBJECTS='1';$env:POSEIDON_REFORGER_STREAM='1'
$env:WGR_GRASS='0';$env:WGR_TEMPORAL='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='1'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$taskPort=$listener.LocalEndpoint.Port;$listener.Stop()
$taskArgs=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$taskPort",'--diag',('"'+$taskOut+'"'),'--diag-seed','1337','--test-mission',('"'+(Join-Path $taskRoot 'tests/perf/missions/perf_field.eden')+'"'),'--test-world','"C:/Program Files (x86)/Steam/steamapps/common/Arma Reforger/addons"','--test-world-hour','16','--log-file',('"'+(Join-Path $taskOut 'engine.log')+'"'))
if($ExperimentalAi){
 $taskAddonRoot=Join-Path $taskRoot 'build/dec-ai-test/@OP_DecAiExperimental/AddOns'
 if(!(Test-Path -LiteralPath (Join-Path $taskAddonRoot 'op_dec_ai_experimental.pbo'))){throw 'Run Prepare-DecAiTestAddon.ps1 first.'}
 $taskArgs+=@('--addon-root',('"'+$taskAddonRoot+'"'))
}
$taskP=$null;$taskClient=$null;$taskPassed=$false
function Send($command){
 $taskWriter.WriteLine(($command|ConvertTo-Json -Compress));$deadline=[DateTime]::UtcNow.AddSeconds(30)
 do{if([DateTime]::UtcNow -gt $deadline){throw 'Harness deadline.'};$line=$taskReader.ReadLine();if(!$line){throw 'Disconnected.'};$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
 if(!$reply.ok){throw $line};return $reply
}
function Eval($code){return (Send @{cmd='eval';code=$code}).result}
try{
 $taskP=Start-Process -FilePath (Join-Path $taskGame 'OpenPoseidon.exe') -WorkingDirectory $taskGame -WindowStyle Hidden -ArgumentList $taskArgs -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(120)
 do{if($taskP.HasExited){throw 'Game exited during startup.'};$taskClient=[Net.Sockets.TcpClient]::new();try{$taskClient.Connect('127.0.0.1',$taskPort)}catch{$taskClient.Dispose();$taskClient=$null};if(!$taskClient){if([DateTime]::UtcNow -gt $deadline){throw 'Startup deadline.'};Start-Sleep -Milliseconds 250}}while(!$taskClient)
 $stream=$taskClient.GetStream();$stream.ReadTimeout=30000;$taskReader=[IO.StreamReader]::new($stream);$taskWriter=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$taskWriter.AutoFlush=$true
 Start-Sleep -Seconds 12
 if((Eval 'triSceneReady') -ne '"OK"'){throw 'Scene not ready.'}
 $taskAi=@{}
 $inspect=Send @{cmd='diag_inspect';unit='player'}
 $groups=Send @{cmd='diag_groups'}
 $null=Send @{cmd='diag_mark';text='dec-integration-smoke'}
 $pause=Send @{cmd='diag_pause'};if(!$pause.paused){throw 'Pause refused.'}
 $time0=[double](Eval 'time');Start-Sleep -Milliseconds 300;$time1=[double](Eval 'time')
 if($time0 -ne $time1){throw 'Paused simulation clock changed.'}
 $null=Send @{cmd='diag_step';ticks=5};Start-Sleep -Milliseconds 600;$time2=[double](Eval 'time')
 $advance=$time2-$time1;if($advance -lt .075 -or $advance -gt .095){throw "Five fixed ticks advanced $advance seconds."}
 $null=Send @{cmd='diag_resume'}
 $null=Send @{cmd='screenshot';path=(Join-Path $taskOut 'scene.png')}
 Start-Sleep -Seconds 2
 $null=Send @{cmd='exit'}
 if(!$taskP.WaitForExit(30000) -or $taskP.ExitCode -ne 0){throw 'Normal exit failed.'}
 $lines=@(Get-Content -LiteralPath (Join-Path $taskOut 'events.jsonl')|ForEach-Object{$_|ConvertFrom-Json})
 $summary=Get-Content -LiteralPath (Join-Path $taskOut 'summary.json') -Raw|ConvertFrom-Json
 if(!($lines|Where-Object{$_.ev -eq 'mark' -and $_.text -eq 'dec-integration-smoke'})){throw 'Marker absent from event log.'}
 if((Get-Content -LiteralPath (Join-Path $taskGame 'DEPLOYED-FROM.txt') -Raw) -ne $taskStamp){throw 'Deployment changed during smoke.'}
 if(Select-String -LiteralPath (Join-Path $taskOut 'engine.log') -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet){throw 'Runtime failure logged.'}
 if($ExperimentalAi){
  $receipt=Select-String -LiteralPath (Join-Path $taskOut 'engine.log') -SimpleMatch 'CfgAIFork (AIHunt): found hunting=1 grenadeInterval=5.0' | Select-Object -First 1
  if(!$receipt){throw 'Actual C++ AI configuration receipt absent.'}
  $taskAi=@{archiveSha256=(Get-FileHash -LiteralPath (Join-Path $taskAddonRoot 'op_dec_ai_experimental.pbo')).Hash;runtimeReceipt=$receipt.Line;scope='Addon mount and hunting/interval config consumption; firefight behavior not exercised'}
 }
 @{passed=$true;installed=$taskStamp.Trim();files=@($taskFiles);events=$lines.Count;summary=$summary;pausedTime=$time1;stepAdvance=$advance;inspect=$inspect;groups=$groups;experimentalAi=$taskAi;exit=$taskP.ExitCode}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
 $taskPassed=$true;Write-Host "Installed diagnostics PASS: $taskOut"
}finally{
 if($taskP -and !$taskP.HasExited){try{if($taskClient){$null=Send @{cmd='exit'}}}catch{};if(!$taskP.WaitForExit(10000)){Stop-Process -Id $taskP.Id -Force}}
 if($taskClient){$taskClient.Dispose()}
}
