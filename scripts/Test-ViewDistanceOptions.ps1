<# Installed normal Options -> Game view-distance input, isolated profile. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$root=Split-Path $PSScriptRoot -Parent
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $root ('build/view-distance-options/'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Path $out -Force
$receipt=@{installed=$stamp;passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;$line|Add-Content (Join-Path $out 'harness.jsonl');return $reply}
function Exec($code){$null=Send @{cmd='exec';code=$code};Start-Sleep -Milliseconds 250}
function Text($id){return ([string](Send @{cmd='eval';code=('triControlText '+$id)}).result).Trim('"')}
try{
 $env:POSEIDON_USER_DIR=Join-Path $out 'user';$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'),"preferredViewDistance=700;`ntextLanguage=""English"";`nvoiceLanguage=""English"";`n")
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log';$stdout=Join-Path $out 'stdout.log';$stderr=Join-Path $out 'stderr.log'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--lang','English','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(180)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 do{$idd=([string](Send @{cmd='eval';code='triDisplay'}).result).Trim('"');Require ([DateTime]::UtcNow-lt$deadline) 'Menu deadline';Start-Sleep -Milliseconds 250}while([int]$idd-lt0)
 Exec 'triSetLanguage "English"';Exec 'triClickText "OPTIONS"';Exec 'triClickText "Game"'
 Require ((Text 522)-eq'700 m') 'Initial Options value differs'
 Exec 'triSendKey 81';Exec 'triSendKey 81';Exec 'triSendKey 79'
 $receipt.afterRight=Text 522;Require ($receipt.afterRight-eq'800 m') 'Right arrow did not select800m'
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'options-800m.png')}
 Exec 'triSendKey 79';$receipt.afterNext=Text 522;Require ($receipt.afterNext-eq'900 m') 'Next arrow did not select900m'
 Exec 'triSendKey 80';$receipt.afterLeft=Text 522;Require ($receipt.afterLeft-eq'800 m') 'Left arrow did not return800m'
 Exec 'triSendKey 41';$receipt.savedConfig=Get-Content (Join-Path $env:POSEIDON_USER_DIR 'game.cfg') -Raw
 Require ($receipt.savedConfig-match'preferredViewDistance\s*=\s*800(?:\.0+)?\s*;') '800m not persisted'
 Exec 'triClickText "Game"';Require ((Text 522)-eq'800 m') 'Reopened Options lost800m'
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Normal exit failed'
 Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed pair changed'
 $receipt.passed=$true;Write-Host ('Evidence: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()};$receipt|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'result.json')
}
