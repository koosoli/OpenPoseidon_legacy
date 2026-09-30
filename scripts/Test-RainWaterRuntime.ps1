# Installed real-rain runoff lifecycle. No simulation water setters or synthetic stamps.
[CmdletBinding()]
param([string]$Label='rainwater',[ValidateSet('noe','eden','abel','cain')][string]$World='noe',
      [ValidateRange(2000,12000)][int]$ViewDistance=2000,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Preserve the running game.' }
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root ('build/rain-water-runtime/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$profile=Join-Path $output 'user'; New-Item -ItemType Directory -Force $profile | Out-Null
$log=Join-Path $output 'engine.log'
$result=[ordered]@{status='running';gates=@{};captures=@{};limitations='Actual source terrain and real rain; this lifecycle does not establish all-map appearance, crater editing, ground-mesh collision, or swimming/buoyancy.'}
function Require($condition,[string]$message) { if (!$condition) { throw $message } }
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    return @{stamp=$stamp;exe=(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$result.before=Pair
$keys=@('POSEIDON_USER_DIR','POSEIDON_PLAYER_GROUND_TRACE','POSEIDON_UNIFORM_WET_TRACE','POSEIDON_SAND_TRACE','POSEIDON_MUD_TRACE','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','WGR_TERRAIN_PUDDLE_WETNESS','WGR_UNIFORM_WET','WGR_RAIN_WATER','WGR_RAIN_WATER_TRACE')
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
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$null,'Process')}
    $env:POSEIDON_USER_DIR=$profile;$env:WGR_RAIN_WATER_TRACE='1'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $mission=Join-Path $output ('field.'+$World);New-Item -ItemType Directory -Force $mission|Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_sand.noe/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd',"$ViewDistance",'--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','14','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt');$null=$p.Handle
    $until=[DateTime]::UtcNow.AddSeconds(120)
    do {
        Health;$client=[Net.Sockets.TcpClient]::new()
        try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null}
        if(!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}
    } while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission not ready.'
    Exec 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0'
    $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    $result.gates.initial=Send @{cmd='dev_rain_water';action='state'}
    Require ($result.gates.initial.volume -eq 0) 'Fresh mission has stale rainwater.'
    Exec '0 setOvercast 1; 0 setRain 1; setAccTime 8'
    $until=[DateTime]::UtcNow.AddSeconds(70)
    do {
        Start-Sleep -Milliseconds 600;$state=Send @{cmd='dev_rain_water';action='state'}
        Require ([DateTime]::UtcNow -lt $until) 'Actual rain did not create a field.'
    } while($state.width -lt 2 -or $state.rainVolume -lt 500000)
    Exec 'setAccTime 0';$wet=Send @{cmd='dev_rain_water';action='state'};$result.gates.wet=$wet
    $result.gates.weather=Send @{cmd='weather_visibility'}
    Require ($wet.volume -gt 0 -and $wet.revision -gt 0) 'Actual wet field has no water.'
    $net=$wet.rainVolume-$wet.infiltrationVolume-$wet.evaporationVolume-$wet.outletVolume
    Require ([Math]::Abs($wet.volume-$net) -lt [Math]::Max(1,$wet.rainVolume*.001)) 'Installed water budget does not conserve volume.'
    $samples=@();$spacing=[double]$wet.spacing
    $cx=[Math]::Round(2675/$spacing)*$spacing;$cz=[Math]::Round(5125/$spacing)*$spacing
    foreach($dz in -5..5) {foreach($dx in -5..5) {
        $x=$cx+$dx*$spacing;$z=$cz+$dz*$spacing
        $sample=Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z}
        $samples+=@{x=$x;z=$z;sample=$sample}
    }}
    $result.gates.samples=$samples
    $target=@($samples|Where-Object {$_.sample.valid -and $_.sample.depth -gt .001 -and [Math]::Abs($_.sample.height-$_.sample.depth-$_.sample.terrainY) -lt .15}|Sort-Object {$_.sample.depth} -Descending)[0]
    Require ($null -ne $target) 'No supported standing-water sample near actual player.'
    $result.gates.selectedTarget=$target
    $x=$target.x;$z=$target.z;$y=[double]$target.sample.height
    Require ((Eval ('triFreeFlyPose "'+$x+' '+($z-24)+' '+($y+9)+' 0 -20.55"')) -ceq 'OK') 'Water camera refused.'
    Capture 'real-rain-standing-water'
    $paused=Send @{cmd='dev_rain_water';action='state'}
    Require ($paused.revision -eq $wet.revision -and $paused.generation -eq $wet.generation -and $paused.volume -eq $wet.volume) 'Pause/camera/sample changed simulation water.'
    # Original weather deliberately regenerates rain under storm cloud. Leave
    # the storm before measuring drainage; query the actual liquid rain gate.
    Exec '0 setOvercast 0.6; 0 setRain 0; setAccTime 1'
    Start-Sleep -Seconds 1;Exec 'setAccTime 0'
    $stopWeather=Send @{cmd='weather_visibility'};Require ($stopWeather.liquidRain -eq 0) 'Storm has not actually stopped raining.'
    $stop=Send @{cmd='dev_rain_water';action='state'};$result.gates.rainStopped=$stop
    $result.gates.localRainStopped=Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z}
    Exec 'setAccTime 8'
    Start-Sleep -Seconds 16;Exec 'setAccTime 0'
    $drain=Send @{cmd='dev_rain_water';action='state'};$result.gates.drain=$drain
    Require ($drain.rainVolume -eq $stop.rainVolume -and $drain.volume -lt $stop.volume -and $drain.volume -gt 0) 'Actual rainwater did not gradually drain after rain.'
    $result.gates.localAfterRain=Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z}
    Capture 'after-rain-retained-water'
    Exec '0 setOvercast 0; setAccTime 8'
    Start-Sleep -Seconds 16;Exec 'setAccTime 0'
    $sun=Send @{cmd='dev_rain_water';action='state'};$result.gates.sun=$sun
    Require ($sun.evaporationVolume -gt $drain.evaporationVolume -and $sun.volume -lt $drain.volume) 'Clear-day weather did not evaporate retained water.'
    $result.gates.localSun=Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z}
    Capture 'sun-drying-water'
    Require (Select-String -Path $log -Pattern 'RAIN_WATER_INIT' -Quiet) 'Actual world field initialization trace missing.'
    Require (Select-String -Path $log -Pattern 'Rain water grid:.*accepted=true' -Quiet) 'Actual renderer snapshot acceptance missing.'
    CloseOwned;$result.status='real-rain-runoff-lifecycle-passed-visual-review-required'
} catch {$result.status='failed';$result.error=$_.Exception.Message;throw}
finally {
    if($p -and !$p.HasExited) {try {CloseOwned} catch {if(!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if($client) {$client.Dispose()}
    $result.after=Pair;Require ($result.before.stamp -ceq $result.after.stamp -and $result.before.exe -ceq $result.after.exe -and $result.before.dll -ceq $result.after.dll) 'Installed pair changed during test.'
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $output 'result.json')
    Write-Host "Actual rainwater evidence: $output"
}
