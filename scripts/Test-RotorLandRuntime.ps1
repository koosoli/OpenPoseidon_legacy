# Installed land-downwash falsifier. Physics/AI fly the aircraft between samples.
[CmdletBinding()]
param([ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-land-ring',
      [double]$SandX=2675,[double]$SandZ=5125,
      [Nullable[double]]$SeaX,[Nullable[double]]$SeaZ,
      [ValidateRange(0.1,1)][double]$SnowDepth=0.35,
      [ValidateRange(6,15)][int]$ObserveSeconds=8,
      [ValidateRange(12,24)][double]$HoverHeight=18,
      [switch]$ManualPilot,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run through scripts/with-game-lock.sh.'}
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) {throw 'Preserve the running game.'}
if (($null -eq $SeaX) -ne ($null -eq $SeaZ)) {throw 'Supply both SeaX and SeaZ, or neither.'}
$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) {if (!$condition) {throw $message}}
function Number([double]$value) {
    Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite fixture number.'
    return $value.ToString('R',$culture)
}
function Pair {
    # Provenance is the first installed file read.
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    Require ($stamp.Length -gt 0) 'Empty installed provenance.'
    $files=@('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file=Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length;writtenUtc=$file.LastWriteTimeUtc.ToString('o')}
    }
    return @{stamp=$stamp;files=@($files)}
}
$before=Pair
$output=Join-Path $root ('build/rotor-land/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $output 'user';New-Item -ItemType Directory -Force $profile | Out-Null
$log=Join-Path $output 'engine.log'
$mission=Join-Path $root 'tests/perf/missions/perf_sand.noe'
Require (Test-Path -LiteralPath $mission) 'Stock Noe fixture mission missing.'
$result=[ordered]@{status='running';before=$before;gates=@{};captures=@{};
    limitations='Actual land emission/lifecycle gates only. Stills require visual review; this does not numerically accept pre-existing water downwash or all-map source coverage.'}
@{sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
  roofHelperSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
  installed=$before;mission=$mission;lockOwner=$env:LOCK_OWNER;requestedSand=@($SandX,$SandZ);
  helicopter='stock UH60';pilot='stock SoldierWB in independent west group';manualPilot=[bool]$ManualPilot;hoverHeight=$HoverHeight;
  observationSimulationSeconds=$ObserveSeconds;seaLevel=0;seaScope='Stock Noe default sea datum, bounded runtime terrain queries';
  relocation='One explicit setPosASL between stages; no setVelocity, forced RPM, repeated position pinning, synthetic particle or terrain stamps'} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES',
    'POSEIDON_SNOWLINE','POSEIDON_ROTOR_LAND','POSEIDON_ROTOR_LAND_TRACE','POSEIDON_WIND_OVERRIDE',
    'WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_RAIN_WATER_TRACE')
$saved=@{};foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$writer=$null;$reader=$null;$script:heliId=$null;$script:heldUp=$false
function Health {
    Require ($p -and !$p.HasExited) 'Owned game exited unexpectedly.'
    if ((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator' -Quiet)) {throw 'Installed validation/script failure.'}
}
function Send($command) {
    Health;$line=$command | ConvertTo-Json -Compress
    $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$writer.WriteLine($line)
    do {
        $replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.'
        $replyLine | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$reply=$replyLine | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    Require $reply.ok $replyLine;return $reply
}
function Exec([string]$code) {$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code) {
    $display=([string](Send @{cmd='eval';code=$code}).result).Trim()
    if ($display.StartsWith('"')) {return $display.Substring(1,$display.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Capture([string]$stage) {
    $path=Join-Path $output ($stage+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    Require ((Get-Item -LiteralPath $path).Length -gt 100) 'Screenshot empty.'
    $result.captures[$stage]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
}
function Height([double]$x,[double]$z) {Get-TerrainPuddleFixtureHeight {param($request) Send $request} $x $z}
function View([double]$x,[double]$z,[double]$ground) {
    if ($ManualPilot) {
        # Scripted camera preserves ordinary helicopter keyboard control.
        # Camera/target commands use AGL, unlike the stored ASL fixture height.
        $cameraX=$x+30;$cameraZ=$z-36;$cameraGround=Height $cameraX $cameraZ
        Exec ('rotorCamera camSetPos ['+(Number $cameraX)+','+(Number $cameraZ)+','+(Number ($ground+27-$cameraGround))+']; rotorCamera camSetTarget ['+(Number $x)+','+(Number $z)+',18]; rotorCamera camCommit 0')
        return
    }
    $pose=@(($x+30),($z-36),($ground+27),320,-23) | ForEach-Object {Number $_}
    Require ((Eval ('triFreeFlyPose "'+($pose -join ' ')+'"')) -ceq 'OK') 'Free-fly fixture camera refused.'
}
function Rows([int]$from=0) {
    if (!(Test-Path -LiteralPath $log)) {return @()}
    $lines=@(Get-Content -LiteralPath $log)
    $pattern='ROTOR_LAND object=(\d+) x=([\d.eE+-]+) y=([\d.eE+-]+) z=([\d.eE+-]+) rpm=([\d.eE+-]+) height=([\d.eE+-]+) emitted=(\d+) snow=(\d+) sheltered=(\d+) live=(\d+) rain=([\d.eE+-]+)'
    $rows=@(for($i=$from;$i -lt $lines.Count;++$i) {
        if ($lines[$i] -match $pattern) {
            $m=$Matches.Clone();if ($null -ne $script:heliId -and $m[1] -ne $script:heliId) {continue}
            $values=2..6 | ForEach-Object {[double]::Parse($m[$_],$culture)}
            $rain=[double]::Parse($m[11],$culture)
            foreach($v in @($values)+@($rain)) {Require (![double]::IsNaN($v) -and ![double]::IsInfinity($v)) 'Nonfinite rotor trace.'}
            $row=@{line=$lines[$i];lineNumber=$i+1;object=$m[1];x=$values[0];y=$values[1];z=$values[2];rpm=$values[3];height=$values[4];
                emitted=[int]$m[7];snow=[int]$m[8];sheltered=[int]$m[9];live=[int]$m[10];rain=$rain}
            Require ($row.rpm -gt .2 -and $row.rpm -le 1.001 -and $row.height -ge 0 -and $row.height -lt 30) 'Rotor trace outside physical emission gates.'
            Require ($row.emitted -le 8 -and $row.snow -le $row.emitted -and $row.sheltered -le 8 -and $row.live -le 128 -and $rain -ge 0 -and $rain -le 1) 'Rotor particle/probe bounds exceeded.'
            $row
        }
    });return $rows
}
function LineCount {return @(Get-Content -LiteralPath $log).Count}
function Observe([string]$stage,[double]$x,[double]$z,[bool]$mustTrace=$true) {
    $first=LineCount;$start=[double](Eval 'time');$until=[DateTime]::UtcNow.AddSeconds($ObserveSeconds*3+10)
    $poses=@();Exec 'setAccTime 1'
    do {
        Start-Sleep -Milliseconds 250;Health;$position=Eval 'getPosASL rotorHeli'
        Require ($position.Count -eq 3 -and (Eval 'alive rotorHeli')) 'Aircraft lost during actual physics interval.'
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$x,2)+[Math]::Pow($position[1]-$z,2))
        Require ($distance -le 8) 'Actual AI hover left the bounded source fixture; not an emission defect.'
        $poses+=,@($position);$now=[double](Eval 'time')
        Require ([DateTime]::UtcNow -lt $until) 'Simulation observation timed out.'
    } while ($now-$start -lt $ObserveSeconds)
    Exec 'setAccTime 0';Start-Sleep -Milliseconds 300;$rows=@(Rows $first)
    if ($mustTrace) {Require ($rows.Count -ge 3) 'No sustained actual rotor/RPM trace; fixture unavailable, not accepted.'}
    foreach($row in $rows) {Require ([Math]::Sqrt([Math]::Pow($row.x-$x,2)+[Math]::Pow($row.z-$z,2)) -le 8) 'Trace belongs outside requested fixture.'}
    $result.gates[$stage]=@{startTime=$start;endTime=$now;rows=$rows;actualPositions=$poses;engineOn=(Eval 'isEngineOn rotorHeli')}
    Capture $stage;return $rows
}
function Place([double]$x,[double]$z,[double]$height) {
    $ground=Height $x $z;$xz=(Number $x)+','+(Number $z)
    Exec ('rotorHeli setPosASL ['+$xz+','+(Number ($ground+$height))+']; rotorHeli flyInHeight '+(Number $height))
    if ($ManualPilot) {
        # Ordinary Q input refreshes the keyboard height helper after the one
        # stage relocation. Rotor RPM/velocity/physics remain untouched.
        $null=Send @{cmd='key';sc=20;hold=$true};$script:heldUp=$true
        Exec 'setAccTime 1';Start-Sleep -Milliseconds 250
        $null=Send @{cmd='key_up';sc=20};$script:heldUp=$false;Exec 'setAccTime 0'
    } else {Exec ('rotorPilot doMove ['+$xz+',0]')}
    View $x $z $ground;return $ground
}
function AwaitHover([double]$x,[double]$z) {
    $first=LineCount;$until=[DateTime]::UtcNow.AddSeconds(60);Exec 'setAccTime 1'
    $result.gates.hoverWait=@()
    do {
        Start-Sleep -Milliseconds 500;Health
        $result.gates.hoverWait+=@{time=(Eval 'time');position=(Eval 'getPosASL rotorHeli');engineOn=(Eval 'isEngineOn rotorHeli')}
        $rows=@(Rows $first | Where-Object {
            [Math]::Abs($_.x-$x) -le 8 -and [Math]::Abs($_.z-$z) -le 8 -and $_.height -ge 8 -and $_.height -lt 28 -and $_.rpm -ge .65})
        Require ([DateTime]::UtcNow -lt $until) 'Stock UH60 did not establish an actual bounded hover; no synthetic pinning fallback.'
    } while ($rows.Count -lt 2)
    $script:heliId=$rows[-1].object;Exec 'setAccTime 0';return $rows[-1]
}
function CloseOwned {
    if ($script:heldUp) {$null=Send @{cmd='key_up';sc=20};$script:heldUp=$false}
    Require ($writer -and $reader) 'Harness not connected for normal shutdown.'
    Exec 'setAccTime 1';$null=Send @{cmd='exit'}
    Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game did not quit normally with exit zero.'
    Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Normal shutdown proof missing.'
    $result.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys) {Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
    $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_ROTOR_LAND='1';$env:POSEIDON_ROTOR_LAND_TRACE='1';$env:POSEIDON_SNOWLINE='off'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    . (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt');$null=$p.Handle
    $until=[DateTime]::UtcNow.AddSeconds(120)
    do {
        Health;$client=[Net.Sockets.TcpClient]::new();try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null}
        if (!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}
    } while (!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Stock scene not ready.'
    Exec 'player allowDamage false; 0 setFog 0; 0 setRain 0; 0 setOvercast 0; setAccTime 0; setDate [1985,6,21,16,0]'
    $null=Send @{cmd='dev_snow';action='disable'}
    # Clear startup overrides above; leave densityOverride at its ordinary
    # automatic default. The harness density setter accepts only [0,1].
    $null=Send @{cmd='weather_particles';action='set';mode='particle';snowflakes=$false}
    $sand=Send @{cmd='dev_sand';action='sample';x=$SandX;z=$SandZ};Require $sand.sourceEligible 'Requested actual stock source is not sand.';$result.gates.sandSource=$sand
    $ground=Height $SandX $SandZ;$xz=(Number $SandX)+','+(Number $SandZ)
    Exec ('rotorHeli="UH60" createVehicle ['+$xz+',0]; rotorHeli allowDamage false; rotorHeli setFuel 0; rotorHeli engineOn false; rotorGroup=createGroup west; "SoldierWB" createUnit [['+$xz+',0],rotorGroup,"rotorPilot=this; this allowDamage false; this setBehaviour ""CARELESS""; this setCombatMode ""BLUE"""]; rotorPilot moveInDriver rotorHeli; rotorHeli setPos ['+$xz+',0]; doStop rotorPilot; rotorHeli engineOn false')
    Require ((Eval 'typeOf rotorHeli') -ceq 'UH60' -and (Eval 'typeOf driver rotorHeli') -ceq 'SoldierWB') 'Real stock helicopter/crew identity missing.'
    if ($ManualPilot) {
        Exec ('selectPlayer rotorPilot; rotorPilot action ["AUTOHOVER",rotorHeli]; rotorCamera="camera" camCreate ['+$xz+',0]; rotorCamera cameraEffect ["internal","back"]')
        Require ((Eval 'format ["%1",player]') -ceq (Eval 'format ["%1",rotorPilot]')) 'Actual controlled pilot identity differs.'
    }
    $result.gates.object=Eval 'format ["%1",rotorHeli]';View $SandX $SandZ $ground
    $rows=@(Observe 'engine-off' $SandX $SandZ $false);Require (!$result.gates['engine-off'].engineOn -and $rows.Count -eq 0) 'Stopped rotor emitted land puffs.'
    Exec 'rotorHeli setFuel 1; rotorHeli engineOn true';$ground=Place $SandX $SandZ $HoverHeight
    $result.gates.actualHover=AwaitHover $SandX $SandZ
    $rows=@(Observe 'dry-sand' $SandX $SandZ);Require (@($rows | Where-Object {$_.emitted -gt 0 -and $_.snow -eq 0 -and $_.rain -lt .05}).Count -gt 0) 'Actual dry sand hover produced no dust positive control.'
    $pauseTime=[double](Eval 'time');$pausePose=Eval 'getPosASL rotorHeli';$from=LineCount;Capture 'paused-a';Start-Sleep -Seconds 5;Capture 'paused-b'
    $afterTime=[double](Eval 'time');$afterPose=Eval 'getPosASL rotorHeli'
    Require ([Math]::Abs($afterTime-$pauseTime) -lt .0001 -and @(Rows $from).Count -eq 0) 'Pause advanced simulation or emitted land particles.'
    for($i=0;$i -lt 3;++$i) {Require ([Math]::Abs($afterPose[$i]-$pausePose[$i]) -lt .001) 'Paused aircraft moved.'}
    $result.gates.pause=@{beforeTime=$pauseTime;afterTime=$afterTime;beforePosition=$pausePose;afterPosition=$afterPose}
    $null=Send @{cmd='dev_snow';action='enable'};$null=Send @{cmd='dev_snow';action='deposit';metres=$SnowDepth}
    $result.gates.snowState=Send @{cmd='dev_snow';action='state'};Require ($result.gates.snowState.depth -ge $SnowDepth-.0001) 'Actual deposited snow is absent.'
    $rows=@(Observe 'deposited-snow' $SandX $SandZ);Require (@($rows | Where-Object {$_.snow -gt 0}).Count -gt 0) 'Snow positive control emitted no actual snow puffs.'
    $null=Send @{cmd='dev_snow';action='disable'}
    # Twenty-five real audited stock roofs cover the bounded <=11.4m spoke ring.
    # Acceptance still requires actual physical hit counts, not this layout guess.
    Exec 'rotorRoofs=[]';$roofProof=@()
    foreach($dx in @(-12,-6,0,6,12)) {foreach($dz in @(-12,-6,0,6,12)) {
        $roof=New-TerrainPuddleStockRoof -SendCommand {param($request) Send $request} -X ($SandX+$dx) -Z ($SandZ+$dz) -RoofLift 3
        Exec 'rotorRoofs=rotorRoofs+[puddleStockRoof]';$roofProof+=,$roof
    }}
    $result.gates.roofObjects=$roofProof;$rows=@(Observe 'roof-covered' $SandX $SandZ)
    Require (@($rows | Where-Object {$_.sheltered -gt 0}).Count -gt 0) 'Roof fixture established no actual blocked airflow rays.'
    Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -eq 0) 'Covered rotor spokes still emitted; retain actual rays and inspect canopy coverage.'
    Exec '{deleteVehicle _x} forEach rotorRoofs';$rows=@(Observe 'roof-removed' $SandX $SandZ)
    Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -gt 0) 'Removed-roof dust positive control failed.'
    # Elevated active rotor must not emit land puffs. One stage relocation only.
    $ground=Place $SandX $SandZ 48;Exec 'setAccTime 1';Start-Sleep -Seconds 4;Exec 'setAccTime 0'
    $position=Eval 'getPosASL rotorHeli';Require ($position[2]-(Height $position[0] $position[1]) -gt 35) 'High-altitude fixture not actually high.'
    $rows=@(Observe 'high-altitude' $SandX $SandZ $false);Require ($rows.Count -eq 0 -and $result.gates['high-altitude'].engineOn) 'High active rotor emitted land puffs.'
    foreach($pose in $result.gates['high-altitude'].actualPositions) {Require ($pose[2]-(Height $pose[0] $pose[1]) -gt 35) 'Aircraft fell into the land emission window during high-altitude control.'}
    $ground=Place $SandX $SandZ $HoverHeight;$null=AwaitHover $SandX $SandZ
    Exec '0 setOvercast 1; 0 setRain 1; setAccTime 3';$until=[DateTime]::UtcNow.AddSeconds(90)
    do {Start-Sleep -Milliseconds 400;$weather=Send @{cmd='weather_visibility'};$soil=Send @{cmd='dev_sand';action='state'};Require ([DateTime]::UtcNow -lt $until) 'Real rain failed to establish wet ground.'} while ($weather.liquidRain -lt .95 -or $soil.wetness -lt .3)
    Exec 'setAccTime 0';$result.gates.realWet=@{weather=$weather;soil=$soil;water=(Send @{cmd='dev_rain_water';action='sample';x=$SandX;z=$SandZ})}
    $rows=@(Observe 'real-rain-wet' $SandX $SandZ);Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -eq 0) 'Wet actual rainy ground emitted dry dust.'
    # Read-only shallow-sea search; no water material/control setter is used.
    $sea=$null
    if ($null -ne $SeaX) {$candidates=@(@([double]$SeaX,[double]$SeaZ))} else {
        $candidates=@(for($i=1;$i -le 10;++$i) {,@(($SandX-$i*128),$SandZ)})
    }
    foreach($candidate in $candidates) {
        $bath=Send @{cmd='water_bathymetry';x=$candidate[0];z=$candidate[1]}
        foreach($point in $bath.samples) {
            if ($point[2] -ge -.1 -or $point[2] -lt -8) {continue}
            $near=@($bath.samples | Where-Object {[Math]::Abs($_[0]-$point[0]) -le 32 -and [Math]::Abs($_[1]-$point[1]) -le 32})
            if ($near.Count -eq 25 -and @($near | Where-Object {$_[2] -ge -.03}).Count -eq 0) {$sea=@([double]$point[0],[double]$point[1]);break}
        }
        if ($sea) {break}
    }
    Require ($null -ne $sea) 'No verified shallow sea footprint found; supply audited SeaX/SeaZ. Water behavior is not accepted by absence.'
    $result.gates.seaTerrain=@{centre=$sea;terrainHeight=(Height $sea[0] $sea[1]);proof='All25 actual terrain samples in 64m square below stock Noe sea datum'}
    $ground=Place $sea[0] $sea[1] $HoverHeight;$null=AwaitHover $sea[0] $sea[1]
    $rows=@(Observe 'sea-existing-water-path' $sea[0] $sea[1]);Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -eq 0) 'Sea footprint emitted land particles.'
    CloseOwned;$result.status='land-emission-lifecycle-passed-visual-and-existing-water-review-required'
} catch {$result.status='failed';$result.error=$_.Exception.Message;throw}
finally {
    if ($p -and !$p.HasExited) {try {CloseOwned} catch {
        $result.status='failed';$result.cleanupError=$_.Exception.Message
        if (!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}
    }}
    if ($client) {$client.Dispose()}
    try {
        $after=Pair;$result.after=$after
        Require (($before | ConvertTo-Json -Depth 6 -Compress) -ceq ($after | ConvertTo-Json -Depth 6 -Compress)) 'Installed pair/provenance changed during campaign.'
    } catch {$result.status='failed';$result.provenanceError=$_.Exception.Message}
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Land rotor evidence: $output"
    if ($result.provenanceError) {throw $result.provenanceError}
    if ($result.cleanupError -and !$result.error) {throw $result.cleanupError}
}
