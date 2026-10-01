# Installed native terrain edits and actual-rain conservative fine water.
# Invoke through with-game-lock.sh. No water setters or synthetic crater geometry.
[CmdletBinding()]
param([switch]$SelfTest,[switch]$OnlyOn,
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='native-crater',
      [double]$X=5012.5,[double]$Z=4662.5,
      [ValidateRange(30,600)][int]$RainSeconds=120,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) {if(!$condition) {throw $message}}
function Number([double]$value) {
    Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite fixture coordinate.'
    return $value.ToString('R',$culture)
}
function Decode-Eval([string]$display) {
    $display=$display.Trim()
    if($display.StartsWith('"') -and $display.EndsWith('"')) {return $display.Substring(1,$display.Length-2).Replace('""','"')}
    if($display.StartsWith('[')) {return ,(ConvertFrom-Json -InputObject ('{"v":'+$display+'}')).v}
    if($display -cmatch '^(true|false|null|-?\d+(\.\d+)?([eE][+-]?\d+)?)$') {return ConvertFrom-Json -InputObject $display}
    return $display
}
function Restore-Environment([string]$key,$value) {
    if($null -eq $value) {Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    else {[Environment]::SetEnvironmentVariable($key,$value,'Process')}
}
function Assert-Budget($state) {
    foreach($name in @('volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume')) {
        Require ($null -ne $state.$name -and ![double]::IsNaN($state.$name) -and ![double]::IsInfinity($state.$name) -and $state.$name -ge 0) "Invalid actual $name."
    }
    $net=$state.rainVolume-$state.infiltrationVolume-$state.evaporationVolume-$state.outletVolume
    Require ([Math]::Abs($state.volume-$net) -le [Math]::Max(.001,$state.rainVolume*.00001)) 'Actual global water budget does not conserve volume.'
}
function Rain-Ready($weather) {
    foreach($name in @('rain','liquidRain','particleDensity')) {
        Require ($null -ne $weather.$name -and ![double]::IsNaN($weather.$name) -and ![double]::IsInfinity($weather.$name) -and
                 $weather.$name -ge 0 -and $weather.$name -le 1) "Missing/nonfinite actual weather $name."
    }
    Require ($weather.particleSnowflakes -is [bool] -and !$weather.particleSnowflakes -and $weather.particleDensity -eq 0 -and
             $weather.rain -eq $weather.liquidRain) 'Rain fixture requires actual natural liquid rain without particle/snow overrides.'
    # Natural weather varies below the requested target. The real rain-volume,
    # local filling, horizontal geometry and mass gates establish the outcome.
    return $weather.liquidRain -gt .25
}
function Rain-WaitAllowed($weather,[long]$additionalMs) {
    Require ($additionalMs -ge 0) 'Actual rain-wait clock reversed.'
    # Reserve ordinary RPC/pause latency inside the hard 60s actual-time gate.
    return !(Rain-Ready $weather) -and $additionalMs -lt 59000
}
function Assert-Fine($fine,$state,[bool]$enabled) {
    foreach($name in @('readonly','sourceReady','fineActive','valid')) {Require ($fine.$name -is [bool]) "Missing/incorrect typed fine $name."}
    Require ($fine.readonly -eq $true -and $fine.sourceReady -eq $true) 'Actual read-only source is not ready.'
    Require ($fine.fineActive -eq $enabled) 'CPU fine feature gate differs from this process arm.'
    Require ($fine.generation -eq $state.generation -and $fine.revision -eq $state.revision) 'CPU fine snapshot is not this exact field revision.'
    if(!$enabled) {Require (!$fine.valid -and $fine.tiles -eq 0 -and $fine.cells -eq 0 -and !$fine.selected) 'Default-off world unexpectedly refined terrain.';return}
    Require ($fine.valid -eq $true -and $fine.tiles -ge 1 -and $fine.tiles -le 8 -and $fine.cells -eq 1024*$fine.tiles) 'Fine publication lacks bounded complete tiles.'
    Require ($fine.terrainSpacing -eq 6.25 -and $fine.sourceRevision -eq $fine.heightRevision) 'Fine water is not keyed to current actual native terrain.'
    Require ([Math]::Abs($fine.fineVolume-$fine.integratedVolume) -le [Math]::Max(.000001,$fine.fineVolume*.000001)) 'Actual triangle volume does not match held horizontal heads.'
    Require ($null -ne $fine.selected -and $fine.selected.size -eq 6.25 -and $fine.selected.corners.Count -eq 4) 'No real native cell owns the crater point.'
}
function Find-Receipt([string[]]$lines,$fine,$state) {
    # Exact numeric/boolean grammar; unknown output cannot become an acceptance.
    $pattern='Rain water publication: worldToken=(\d+) generation=(\d+) heightRevision=(\d+) terrainRange=(\d+) terrainSpacing=([0-9.eE+-]+) revision=(\d+) flags=(\d+) copiedCoarse=(\d+) copiedFine=(\d+) cachedCoarse=(\d+) cachedFine=(\d+) tiles=(\d+) enabled=(0|1) accepted=(true|false)\s*$'
    $found=$null
    foreach($line in $lines) {if($line -cmatch $pattern) {
        $m=$Matches
        if($m[1] -ceq [string]$fine.worldToken -and [decimal]$m[2] -eq $fine.generation -and [decimal]$m[3] -eq $fine.heightRevision -and
           [int]$m[4] -eq $fine.terrainRange -and [double]::Parse($m[5],$culture) -eq $fine.terrainSpacing -and
           [decimal]$m[6] -eq $fine.revision -and [int]$m[7] -eq 7 -and [int]$m[10] -eq $state.width*$state.height -and
           [int]$m[11] -eq $fine.cells -and [int]$m[12] -eq $fine.tiles -and $m[13] -ceq '1' -and $m[14] -ceq 'true') {$found=$line}
    }}
    Require ($null -ne $found) 'No exact current accepted renderer coarse+fine receipt (unknown/missing grammar fails closed).'
    return $found
}
function Complete-LogLines([string]$text) {
    # A live writer may have emitted only the prefix of its last record.
    # Never offer that unterminated fragment to the strict receipt parser.
    $last=$text.LastIndexOf("`n")
    if($last -lt 0) {return}
    return $text.Substring(0,$last).Split("`n") | ForEach-Object {$_.TrimEnd("`r")}
}
function Read-CompleteLogLines([string]$path) {
    $stream=$null;$reader=$null
    try {
        $stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true)
        return Complete-LogLines ($reader.ReadToEnd())
    } finally {if($reader) {$reader.Dispose()} elseif($stream) {$stream.Dispose()}}
}
function Expect-Failure([scriptblock]$check) {try {& $check} catch {return};throw 'Negative helper case unexpectedly passed.'}
if($SelfTest) {
    $state=[pscustomobject]@{width=513;height=513;generation=5;revision=19;volume=3;rainVolume=5;infiltrationVolume=1;evaporationVolume=.5;outletVolume=.5}
    $fine=[pscustomobject]@{readonly=$true;sourceReady=$true;fineActive=$true;valid=$true;generation=5;revision=19;sourceRevision=71;heightRevision=71;worldToken='5729000000000';terrainRange=2048;terrainSpacing=6.25;tiles=1;cells=1024;fineVolume=2;integratedVolume=2;selected=@{size=6.25;corners=@(1,2,3,4)}}
    Assert-Budget $state;Assert-Fine $fine $state $true
    $line='Rain water publication: worldToken=5729000000000 generation=5 heightRevision=71 terrainRange=2048 terrainSpacing=6.25 revision=19 flags=7 copiedCoarse=0 copiedFine=0 cachedCoarse=263169 cachedFine=1024 tiles=1 enabled=1 accepted=true'
    $null=Find-Receipt @($line) $fine $state
    foreach($bad in @($line.Replace('revision=19','revision=18'),$line.Replace('accepted=true','accepted=false'),$line.Replace('enabled=1','enabled=0'),$line.Replace('enabled=1','enabled=true'),$line.Replace('enabled=1','enabled=2'),$line.Replace('cachedFine=1024','cachedFine=1023'),$line.Replace('cachedCoarse=263169','cachedCoarse=1052676'),$line.Replace('worldToken=5729000000000','worldToken=5729000000001'),($line+' extra=1'),'accepted=true')) {
        Expect-Failure {Find-Receipt @($bad) $fine $state}
    }
    Require (@(Complete-LogLines $line).Count -eq 0) 'Unterminated receipt fragment accepted as a complete record.'
    $complete=@(Complete-LogLines ($line+"`r`n"+'unfinished receipt'));Require ($complete.Count -eq 1 -and $complete[0] -ceq $line) 'Complete/partial line boundary changed.'
    $temp=[IO.Path]::GetTempFileName();$live=$null
    try {
        $live=[IO.FileStream]::new($temp,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
        $bytes=[Text.Encoding]::UTF8.GetBytes($line);$live.Write($bytes,0,$bytes.Length);$live.Flush()
        Expect-Failure {Find-Receipt @(Read-CompleteLogLines $temp) $fine $state}
        $bytes=[Text.Encoding]::UTF8.GetBytes("`r`n"+'unfinished receipt');$live.Write($bytes,0,$bytes.Length);$live.Flush()
        $null=Find-Receipt @(Read-CompleteLogLines $temp) $fine $state
        # The complete-but-unmatched or unterminated newer lines cannot weaken
        # exact current source/revision/count acceptance.
        Expect-Failure {Find-Receipt @(Read-CompleteLogLines $temp) $fine ([pscustomobject]@{width=512;height=513})}
    } finally {if($live) {$live.Dispose()};Remove-Item -LiteralPath $temp -ErrorAction SilentlyContinue}
    $fine.sourceReady=$false;Expect-Failure {Assert-Fine $fine $state $true};$fine.sourceReady=$true
    $fine.integratedVolume=1;Expect-Failure {Assert-Fine $fine $state $true};$fine.integratedVolume=2
    $fine.cells=1023;Expect-Failure {Assert-Fine $fine $state $true};$fine.cells=1024
    $state.volume=4;Expect-Failure {Assert-Budget $state}
    $rain=@{rain=.4923645257949829;liquidRain=.4923645257949829;particleDensity=0;particleSnowflakes=$false}
    Require ((Rain-Ready $rain) -and !(Rain-WaitAllowed $rain 0)) 'Actual natural .492 rain was rejected despite being a valid forcing source.'
    foreach($value in @(0,.249999,.25,.250001)) {$rain.rain=$value;$rain.liquidRain=$value;Require ((Rain-Ready $rain) -eq ($value -gt .25)) 'Natural-rain forcing threshold changed.'}
    $rain.rain=.1;$rain.liquidRain=.1;Require ((Rain-WaitAllowed $rain 0) -and !(Rain-WaitAllowed $rain 59000)) 'Low-rain wait exceeded its bound.'
    foreach($bad in @(@{rain=.8;liquidRain=.8;particleDensity=.1;particleSnowflakes=$false},@{rain=.8;liquidRain=.8;particleDensity=0;particleSnowflakes=$true},@{rain=.4;liquidRain=.8;particleDensity=0;particleSnowflakes=$false},@{rain=[double]::NaN;liquidRain=.8;particleDensity=0;particleSnowflakes=$false},@{liquidRain=.8})) {Expect-Failure {Rain-Ready $bad}}
    Require ((Decode-Eval '"data3d\mc vojakw2.p3d"') -ceq 'data3d\mc vojakw2.p3d') 'SQF quoted path changed.'
    Write-Host 'Fine runtime helper gates PASS (stale key, rejection, incomplete tiles, source gap, triangle mass, budget, SQF path).';return
}
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
Require ([Math]::Abs($X/6.25-[Math]::Round($X/6.25)) -lt .000001 -and [Math]::Abs($Z/6.25-[Math]::Round($Z/6.25)) -lt .000001 -and
         [Math]::Abs($X/25-[Math]::Floor($X/25)-.5) -lt .000001 -and [Math]::Abs($Z/25-[Math]::Floor($Z/25)-.5) -lt .000001) 'Crater centre must be an actual 6.25m native vertex halfway between old 25m nodes.'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root ('build/rain-water-fine/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force $output|Out-Null
. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ([bool]$stamp) 'Empty deployed provenance.'
    return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$before=Pair
$result=[ordered]@{status='running';before=$before;arms=@{};limitations='Default-off CPU feature and opt-in paired fine renderer. Actual geometry/volume/receipts and captures only; appearance, roof mask and all-map flow require separate visual/GPU acceptance. CPU rain forcing applies to whole owners, including beneath roofs.'}
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_RAIN_WATER','WGR_RAIN_WATER_FINE','WGR_RAIN_WATER_TRACE','POSEIDON_RAIN_WATER_FINE','WGR_WEATHER_COVER','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_HDR_ENCODE','WGR_TONEMAP','POSEIDON_UNIFORM_WET_TRACE')
$saved=@{};foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$script:edited=$false;$script:roof=$false
function Health {
    Require (!$p.HasExited) 'Owned game exited unexpectedly.'
    if((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator' -Quiet)) {throw 'Installed game validation/script failure.'}
}
function Send($command) {
    Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath $rpc;$writer.WriteLine($line)
    do {$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath $rpc;$reply=$replyLine|ConvertFrom-Json} while($null -eq $reply.ok)
    Require $reply.ok $replyLine;return $reply
}
function Exec([string]$code) {$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code) {return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Clock {
    $state=Send @{cmd='query';what='play_state'}
    Require ($state.has_player -and $state.player_active -and $state.player_local) 'Actual local controlled player is unavailable.'
    return [long]$state.time_ms
}
function Wait-Sim([int]$seconds) {
    $start=Clock;$until=[DateTime]::UtcNow.AddSeconds([Math]::Max(35,$seconds))
    do {Start-Sleep -Milliseconds 250;Health;Require ([DateTime]::UtcNow -lt $until) 'Actual simulation time failed to advance.'} while((Clock)-$start -lt $seconds*1000)
}
function State {return Send @{cmd='dev_rain_water';action='state'}}
function Await-NaturalRain($weather) {
    $start=Clock;$initial=State;Assert-Budget $initial
    $proof=@{requiredMinimumDensity=.25;hardAdditionalLimitMs=60000;pollCutoffMs=59000;startTimeMs=$start;status='checking-actual-forcing';
             samples=@(@{additionalMs=0;weather=$weather;field=$initial})}
    $arm.gates.rainForcingWait=$proof
    if(!(Rain-Ready $weather)) {
        Exec 'setAccTime 4';$next=$start+5000;$wallUntil=[DateTime]::UtcNow.AddSeconds(45)
        while(Rain-WaitAllowed $weather ((Clock)-$start)) {
            do {
                Start-Sleep -Milliseconds 100;Health;Require ([DateTime]::UtcNow -lt $wallUntil) 'Natural rain wait exceeded bounded wall time.'
                $now=Clock
            } while($now -lt $next -and $now-$start -lt 59000)
            $weather=Send @{cmd='weather_visibility'};$field=State;Assert-Budget $field
            $proof.samples+=@{additionalMs=((Clock)-$start);weather=$weather;field=$field}
            $next=[Math]::Min((Clock)+5000,$start+59000)
        }
        Exec 'setAccTime 0'
    }
    $proof.actualAdditionalMs=(Clock)-$start
    Require ($proof.actualAdditionalMs -le 60000) 'Actual natural rain wait exceeded 60 extra simulation seconds.'
    $weather=Send @{cmd='weather_visibility'};$field=State;Assert-Budget $field
    $proof.finalWeather=$weather;$proof.finalField=$field;$proof.status='bounded-wait-complete'
    Require (Rain-Ready $weather) 'Natural liquid rain forcing did not reach >0.25 within the bounded additional wait.'
    $proof.status='actual-natural-liquid-rain-ready';return $weather
}
function Fine {return Send @{cmd='dev_rain_water';action='fine_state';x=$X;z=$Z}}
function Await-Receipt($fine,$state) {
    $until=[DateTime]::UtcNow.AddSeconds(10)
    $proof=@{status='awaiting-exact-live-receipt';attempts=0;wallLimitSeconds=10;
             worldToken=$fine.worldToken;generation=$fine.generation;heightRevision=$fine.heightRevision;revision=$fine.revision}
    $arm.gates.rendererReceiptWait=$proof
    do {
        Health;$proof.attempts++;$lines=$null
        try {$lines=@(Read-CompleteLogLines $log)} catch {
            if($_.Exception.GetBaseException() -isnot [IO.IOException]) {throw}
            $proof.lastReadError=$_.Exception.Message
        }
        if($null -ne $lines) {
            try {$receipt=Find-Receipt $lines $fine $state;$proof.status='exact-current-receipt-observed';return $receipt}
            catch {$proof.lastMissingReceipt=$_.Exception.Message}
        }
        if([DateTime]::UtcNow -ge $until) {$proof.status='bounded-receipt-wait-exhausted';throw 'No complete exact current renderer receipt within 10 seconds; live log read/missing receipt details retained.'}
        Start-Sleep -Milliseconds 150
    } while($true)
}
function Brush([string]$action='state') {return Send @{cmd='dev_terrain_brush';action=$action;x=$X;z=$Z}}
function Capture([string]$name) {
    Start-Sleep -Milliseconds 400;$path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10);$length=0;$stable=0
    do {Start-Sleep -Milliseconds 150;Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot not fully written.'
        if(Test-Path $path) {$next=(Get-Item -LiteralPath $path).Length;if($next -gt 0 -and $next -eq $length) {$stable++} else {$stable=0};$length=$next}
    } while($stable -lt 3)
    $arm.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
}
function View([double]$x,[double]$z,[double]$y,[double]$az,[double]$el) {
    Require ((Eval ('triFreeFlyPose "'+((@($x,$z,$y,$az,$el)|ForEach-Object {Number $_}) -join ' ')+'"')) -ceq 'OK') 'Actual freefly camera refused.'
}
function Close-Owned {
    if($script:roof) {Remove-TerrainPuddleStockRoof ${function:Send};$script:roof=$false}
    if($script:edited) {$null=Brush 'restore';$script:edited=$false}
    $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game failed normal quit.'
    Require ($p.ExitCode -eq 0 -and (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) 'Owned game lacks normal zero-exit proof.'
    $arm.exitCode=$p.ExitCode
}
try {
    $modes=if($OnlyOn) {@('fine-on')} else {@('default-off','fine-on')}
    foreach($mode in $modes) {
        foreach($key in $keys) {Restore-Environment $key $null}
        $on=$mode -ceq 'fine-on';$armDir=Join-Path $output $mode;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force $profile|Out-Null
        $arm=[ordered]@{status='running';cpuFine=$on;rendererFine=$on;captures=@{};gates=@{}};$result.arms[$mode]=$arm
        $env:POSEIDON_USER_DIR=$profile;$env:WGR_RAIN_WATER_TRACE='1';$env:WGR_RAIN_WATER='1'
        $env:POSEIDON_SNOWLINE='off';$env:POSEIDON_SNOW_TEST_DEPTH='0'
        if($on) {$env:POSEIDON_RAIN_WATER_FINE='1';$env:WGR_RAIN_WATER_FINE='1'}
        [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $mission=Join-Path $armDir 'crater.noe';New-Item -ItemType Directory -Force $mission|Out-Null
        Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_sand.noe/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
        $log=Join-Path $armDir 'engine.log';$rpc=Join-Path $armDir 'harness.jsonl'
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
        $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','14','--log-file',('"'+$log+'"'))
        $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError (Join-Path $armDir 'stderr.txt');$null=$p.Handle
        $until=[DateTime]::UtcNow.AddSeconds(120)
        do {Health;$client=[Net.Sockets.TcpClient]::new();try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null};if(!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}} while(!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Fresh stock mission not ready.'
        Exec 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setTerrainGrid 6.25; setAccTime 0'
        $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
        $original=Brush;$arm.gates.original=$original
        Require ($original.spacing -eq 6.25 -and !$original.baseline -and $original.vertexWorldX -eq $X -and $original.vertexWorldZ -eq $Z -and $original.vertexHeight -gt $original.seaLevel+2) 'Fixture is not a dry native inland actual vertex.'
        $initial=State;Require ($initial.volume -eq 0 -and !$initial.fineActive) 'Fresh mission was not dry/default-unconfigured.'
        View $X ($Z-30) ($original.vertexHeight+13) 0 -22;Capture 'actual-before-edit'
        $paint=Send @{cmd='dev_terrain_brush';action='paint';x=$X;z=$Z;radius=18;delta=-1};$script:edited=$true;$arm.gates.paint=$paint
        Require ($paint.baseline -and $paint.heightRevision -gt $original.heightRevision -and [Math]::Abs($paint.vertexHeight-($original.vertexHeight-1)) -lt .002 -and
                 $paint.terrainRegistered -and $paint.physicsWidth -eq $original.range -and $paint.physicsHeight -eq $original.range -and $paint.physicsSpacing -eq 6.25) 'Actual terrain/Box3D native edit was not committed.'
        $rim=@();foreach($dx in @(-25,0,25)) {foreach($dz in @(-25,0,25)) {if($dx -eq 0 -and $dz -eq 0) {continue}
            $q=Send @{cmd='dev_terrain_brush';action='state';x=($X+$dx);z=($Z+$dz)};$rim+=$q
            Require ($q.vertexHeight -gt $paint.vertexHeight+.1) 'This actual stock slope does not form a lower centre inside the eight sampled rim vertices; choose another native midpoint.'
        }}
        $arm.gates.rim=$rim
        Exec 'setAccTime 1';Wait-Sim 2;Exec 'setAccTime 0'
        # The genuine edit journal is queued before the first liquid input;
        # World deliberately leaves an entirely dry field unconfigured.
        $dry=State;$dryFine=Fine;Assert-Fine $dryFine $dry $false;Assert-Budget $dry;$arm.gates.dryFine=$dryFine
        Require ($dry.volume -eq 0) 'Dry edit invented water.';Capture 'actual-dry-hollow'
        Exec '0 setOvercast 1; 0 setRain 1; setAccTime 4';Wait-Sim $RainSeconds;Exec 'setAccTime 0'
        $weather=Await-NaturalRain (Send @{cmd='weather_visibility'});$arm.gates.wetWeather=$weather
        $wet=State;$fine=Fine;Assert-Budget $wet;Assert-Fine $fine $wet $on;$arm.gates.wet=$wet;$arm.gates.wetFine=$fine
        $sample=Send @{cmd='dev_rain_water';action='sample';x=$X;z=$Z};$arm.gates.wetSample=$sample
        Require ($wet.rainVolume -gt 0 -and $wet.volume -gt 0 -and $sample.valid -and $sample.depth -gt .001) 'Actual rain did not fill the native crater point.'
        if($on) {
            Require ([Math]::Abs($sample.height-$fine.selected.head) -lt .0001 -and [Math]::Abs($sample.depth-$fine.selected.localDepth) -lt .0001) 'Query is not the actual horizontal fine head clipped to its triangles.'
            $cell=$fine.selected;$points=@();$wetPoint=$false;$dryPoint=$false
            foreach($uv in @(@(.001,.001),@(.15,.15),@(.9,.05),@(.05,.9),@(.9,.9))) {
                $qx=$cell.x+$uv[0]*$cell.size;$qz=$cell.z+$uv[1]*$cell.size
                $geometry=Send @{cmd='dev_rain_water';action='fine_state';x=$qx;z=$qz}
                $water=Send @{cmd='dev_rain_water';action='sample';x=$qx;z=$qz}
                Require ($geometry.valid -and $geometry.selected.parent -eq $cell.parent -and $geometry.selected.x -eq $cell.x -and $geometry.selected.z -eq $cell.z -and
                         $geometry.selected.head -eq $cell.head -and [Math]::Abs($water.height-$cell.head) -lt .0001 -and
                         [Math]::Abs($water.depth-$geometry.selected.localDepth) -lt .0001) 'One actual fine owner does not have a horizontal triangle-clipped query surface.'
                if($water.depth -gt .0005) {$wetPoint=$true};if($water.depth -eq 0) {$dryPoint=$true}
                $points+=@{x=$qx;z=$qz;geometry=$geometry.selected;sample=$water}
            }
            Require ($wetPoint -and $dryPoint) 'This hollow does not expose both wet and dry terrain within the selected sloped native cell; clipping proof unavailable.'
            $arm.gates.clippedHorizontalPoints=$points
        }
        View $X ($Z-18) ($paint.vertexHeight+4) 0 -12;Capture 'real-rain-hollow-near'
        View $X $Z ($paint.vertexHeight+35) 0 -89;Capture 'real-rain-hollow-top'
        $pauseTime=Clock;Start-Sleep -Seconds 2;View ($X+350) ($Z-350) ($paint.vertexHeight+100) 315 -10;Capture 'camera-away'
        View $X ($Z-18) ($paint.vertexHeight+4) 0 -12;Capture 'camera-return'
        $same=State;$sameFine=Fine;Require ((Clock) -eq $pauseTime -and $same.revision -eq $wet.revision -and $same.generation -eq $wet.generation -and $same.volume -eq $wet.volume -and $sameFine.fineVolume -eq $fine.fineVolume) 'Pause or camera/sample reseeded/evicted fine water.'
        if($on) {$arm.gates.rendererReceipt=Await-Receipt $fine $wet}
        # Physical roof exposure is a renderer gate, not CPU whole-owner rainfall exclusion.
        $arm.gates.roof=New-TerrainPuddleStockRoof -SendCommand ${function:Send} -X $X -Z $Z -RoofLift 3;$script:roof=$true
        Capture 'covered-hollow-mask-review';$covered=State;Require ($covered.revision -eq $wet.revision -and $covered.volume -eq $wet.volume) 'Roof creation advanced or deleted paused simulation water.'
        Remove-TerrainPuddleStockRoof ${function:Send};$script:roof=$false
        $sea=Send @{cmd='dev_terrain_brush';action='state';x=0;z=0};$seaWater=Send @{cmd='dev_rain_water';action='sample';x=0;z=0};$arm.gates.sea=@{terrain=$sea;water=$seaWater}
        Require ($sea.vertexHeight -le $sea.seaLevel -and $seaWater.depth -le .000001) 'Actual corner sea outlet unexpectedly became standing rainwater (or this map has no corner-sea fixture).'
        Exec '0 setOvercast 0.6; 0 setRain 0; setAccTime 1';Wait-Sim 2;Exec 'setAccTime 0'
        $stopWeather=Send @{cmd='weather_visibility'};Require ($stopWeather.liquidRain -eq 0) 'Actual storm has not stopped raining.'
        $stopped=State;$arm.gates.stopped=$stopped;Exec 'setAccTime 4';Wait-Sim 60;Exec 'setAccTime 0';$drained=State;Assert-Budget $drained
        Require ($drained.rainVolume -eq $stopped.rainVolume -and $drained.volume -lt $stopped.volume -and $drained.volume -gt 0) 'No-input actual water failed gradual drainage without reseeding.'
        $arm.gates.drained=$drained;Capture 'after-rain-retained-hollow'
        $restoreBefore=State;$restored=Brush 'restore';$script:edited=$false;$arm.gates.restored=$restored
        Require (!$restored.baseline -and $restored.heightRevision -gt $paint.heightRevision -and [Math]::Abs($restored.vertexHeight-$original.vertexHeight) -lt .002) 'Original real terrain/collision was not restored.'
        # Same-command receipt observes invalidation before a later main tick
        # is allowed to refresh this exact revised geometry.
        if($on) {Require (!$restored.fineSourceReady) 'Restore transaction did not immediately close stale fine source publication.'}
        Exec 'setAccTime 1';Wait-Sim 2;Exec 'setAccTime 0';$afterRestore=State;$afterFine=Fine;Assert-Fine $afterFine $afterRestore $on;Assert-Budget $afterRestore
        Require ($afterRestore.rainVolume -eq $restoreBefore.rainVolume -and $afterRestore.volume -le $restoreBefore.volume) 'Bed restore invented rainwater mass.'
        $arm.gates.afterRestore=$afterFine;Capture 'actual-restored-bed'
        Close-Owned;$client.Dispose();$client=$null;$p=$null;$arm.status='actual-cpu-and-source-receipt-gates-passed-captures-await-visual-gpu-review'
    }
    $result.status='bounded-native-edit-lifecycle-passed-visual-gpu-acceptance-pending'
} catch {$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=@{line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}}
finally {
    if($p -and !$p.HasExited) {try {Close-Owned} catch {$result.status='failed';$result.cleanupError=$_.Exception.Message;if(!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if($client) {$client.Dispose()};foreach($key in $keys) {Restore-Environment $key $saved[$key]}
    try {$result.after=Pair;Require ($before.stamp -ceq $result.after.stamp -and $before.exe -ceq $result.after.exe -and $before.dll -ceq $result.after.dll) 'Installed pair changed during fixture.'} catch {$result.status='failed';$result.provenanceError=$_.Exception.Message}
    $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Actual fine rainwater evidence: $output"
    if($result.status -eq 'failed') {throw ($result.error+' '+$result.cleanupError+' '+$result.provenanceError)}
}
