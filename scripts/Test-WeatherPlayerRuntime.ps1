# Installed, standard US PLAYER checks through ordinary SDL movement input.
[CmdletBinding()]
param([string]$Label='owner-player',[switch]$ChurchOnly,
      [ValidateRange(2000,12000)][int]$ViewDistance=2000,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Preserve the running game.' }
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root ('build/weather-player/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$profile=Join-Path $output 'user'; New-Item -ItemType Directory -Force $profile | Out-Null
$log=Join-Path $output 'engine.log'
$result=[ordered]@{status='running';gates=@{};captures=@{};limitations='Controlled standard US player on actual Noe stock sand/soil. Other maps and richer materials require separate scenery checks.'}
function Require($condition,[string]$message) { if (!$condition) { throw $message } }
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    return @{stamp=$stamp;exe=(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$result.before=Pair
$keys=@('POSEIDON_USER_DIR','POSEIDON_PLAYER_GROUND_TRACE','POSEIDON_UNIFORM_WET_TRACE','POSEIDON_SAND_TRACE','POSEIDON_MUD_TRACE','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','WGR_TERRAIN_PUDDLE_WETNESS','WGR_UNIFORM_WET')
$saved=@{}; foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$held=$false
function Health {
    Require (!$p.HasExited) 'Owned game exited unexpectedly.'
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator' -Quiet)) { throw 'Installed game validation/script failure.' }
}
function Send($command) {
    Health; $line=$command|ConvertTo-Json -Compress
    $line|Add-Content (Join-Path $output 'harness.jsonl');$writer.WriteLine($line)
    do {$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content (Join-Path $output 'harness.jsonl');$reply=$replyLine|ConvertFrom-Json} while ($null -eq $reply.ok)
    Require $reply.ok $replyLine;return $reply
}
function Exec([string]$code) { $null=Send @{cmd='exec';code=$code} }
function Eval([string]$code) {
    $reply=Send @{cmd='eval';code=$code};$display=([string]$reply.result).Trim()
    if($display.StartsWith('"')) {return $display.Substring(1,$display.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Capture([string]$stage) {
    Start-Sleep -Milliseconds 600
    $path=Join-Path $output ($stage+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(8)
    while(!(Test-Path $path)) {Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    $result.captures[$stage]=$path
}
function Camera([string]$mode) {Exec ('player switchCamera "'+$mode+'"');Start-Sleep -Milliseconds 300}
function ManualWalk([string]$stage,[double]$minimum=4) {
    $start=Eval 'getPosASL player';Exec 'setAccTime 1';$null=Send @{cmd='key';sc=26;hold=$true};$script:held=$true
    $until=[DateTime]::UtcNow.AddSeconds(12)
    do {
        Start-Sleep -Milliseconds 150;$position=Eval 'getPosASL player'
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$start[0],2)+[Math]::Pow($position[1]-$start[1],2))
        Require ([DateTime]::UtcNow -lt $until) 'Ordinary player input did not move the controlled soldier.'
    } while ($distance -lt $minimum)
    $null=Send @{cmd='key_up';sc=26};$script:held=$false;Start-Sleep -Milliseconds 400;Exec 'setAccTime 0'
    $end=Eval 'getPosASL player';$result.gates[$stage]=@{start=$start;end=$end;distance=$distance;pose=(Eval 'getMove player');alive=(Eval 'alive player');input='SDL W held; no AI doMove, direct stamp or animation override'}
    Require $result.gates[$stage].alive 'Controlled player died.'
}
function CloseOwned {
    if($held) {$null=Send @{cmd='key_up';sc=26};$script:held=$false}
    $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game failed normal quit.'
    Require ($p.ExitCode -eq 0) 'Owned game exit was not zero.'
    Require (Select-String -Path $log -Pattern 'Shutdown complete' -Quiet) 'Normal shutdown proof missing.'
    $result.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys) {Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
    $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_PLAYER_GROUND_TRACE='1';$env:POSEIDON_UNIFORM_WET_TRACE='1';$env:POSEIDON_SAND_TRACE='1';$env:POSEIDON_MUD_TRACE='1'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $mission=Join-Path $root 'tests/perf/missions/perf_sand.noe'
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd',"$ViewDistance",'--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt');$null=$p.Handle
    $until=[DateTime]::UtcNow.AddSeconds(120)
    do {
        Health;$client=[Net.Sockets.TcpClient]::new()
        try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null}
        if(!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}
    } while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission not ready.'
    Require ((Eval 'typeOf player') -ceq 'SoldierWB') 'Actual player is not the standard US soldier.'
    if($ChurchOnly) {
        Exec '0 setFog 0; 0 setRain 0; 0 setOvercast 0; setAccTime 0'
        $result.gates.identities=Send @{cmd='stream_identity_probe';ids=@(1328,43298)}
        $result.gates.weather=Send @{cmd='weather_visibility'}
        Require ((Eval 'triFreeFlyPose "2592.94 5422.69 75.80 100 6.2"') -ceq 'OK') 'Owner camera refused.'
        Capture 'owner-nogova-church-far'
        CloseOwned;$result.status='church-scene-captured-identification-required';return
    }
    Exec 'player allowDamage false; 0 setFog 0; 0 setRain 0; 0 setOvercast 0; player setPos [2675,5125,0]; player setDir 90; player setUnitPos "UP"; setAccTime 1'
    $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    Start-Sleep -Seconds 2;Exec 'setAccTime 0';Camera 'EXTERNAL'
    $dry=Send @{cmd='dev_sand';action='sample';x=2675;z=5125};Require $dry.sourceEligible 'Actual spawn is not stock sand.';$result.gates.sandSource=$dry
    Capture 'sand-third-before';ManualWalk 'sand-manual';Capture 'sand-third-after'
    $sand=Send @{cmd='dev_sand';action='state'};Require ($sand.chunks -gt 0) 'Manual player made no sand contacts.';$result.gates.sandState=$sand
    Camera 'INTERNAL';Capture 'sand-first';Camera 'EXTERNAL';Capture 'sand-third-return'
    Exec 'player setPos [4975,4675,0]; player setDir 0; 0 setOvercast 1; 0 setRain 1; setAccTime 4'
    $until=[DateTime]::UtcNow.AddSeconds(75)
    do {
        Start-Sleep -Milliseconds 400;$wet=[double](Eval 'triUniformWetness player');$mud=Send @{cmd='dev_mud';action='state'}
        Require ([DateTime]::UtcNow -lt $until) 'Real rain did not wet player and soil.'
    } while($wet -lt .3 -or $mud.wetness -lt .3)
    Exec 'setAccTime 0';$result.gates.playerWet=$wet;$result.gates.weather=Send @{cmd='weather_visibility'}
    Capture 'wet-uniform-third';ManualWalk 'mud-manual';Capture 'mud-third-after'
    $mud=Send @{cmd='dev_mud';action='state'};Require ($mud.chunks -gt 0) 'Manual player made no mud contacts.';$result.gates.mudState=$mud
    Camera 'INTERNAL';Capture 'mud-first';Camera 'EXTERNAL';Capture 'mud-third-return'
    $null=Eval 'triFreeFlyPose "2592.94 5422.69 75.80 100 6.2"';Capture 'owner-nogova-church'
    CloseOwned;$result.status='functional-player-checks-passed-visual-review-required'
} catch {$result.status='failed';$result.error=$_.Exception.Message;throw}
finally {
    if($p -and !$p.HasExited) {try {CloseOwned} catch {if(!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if($client) {$client.Dispose()}
    $result.after=Pair;Require ($result.before.stamp -ceq $result.after.stamp -and $result.before.exe -ceq $result.after.exe -and $result.before.dll -ceq $result.after.dll) 'Installed pair changed during test.'
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $output 'result.json')
    Write-Host "Actual player evidence: $output"
}
