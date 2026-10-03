# Installed land-downwash falsifier. Physics/AI fly the aircraft between samples.
[CmdletBinding()]
param([switch]$SelfTest,
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-land-ring',
      [double]$SandX=2675,[double]$SandZ=5125,
      [Nullable[double]]$SeaX,[Nullable[double]]$SeaZ,
      [ValidateRange(0.1,1)][double]$SnowDepth=0.35,
      [ValidateRange(6,15)][int]$ObserveSeconds=8,
      [ValidateRange(12,24)][double]$HoverHeight=18,
      [switch]$ManualPilot,
      [switch]$DrawTrace,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
# Audited physical ViewGeometry roof width 4.78717685m; 4m pitch overlaps it.
$canopyOffsets=@(-8,-4,0,4,8)
function Require([bool]$condition,[string]$message) {if (!$condition) {throw $message}}
function Wet-Ready($weather,$soil) {
    foreach($value in @($weather.liquidRain,$weather.rain,$soil.wetness)) {
        if($null -eq $value -or $value -isnot [ValueType] -or $value -is [bool] -or
           [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or $value -lt 0 -or $value -gt 1) {return $false}
    }
    if($weather.particleSnowflakes -ne $false -or [Math]::Abs($weather.liquidRain-$weather.rain) -gt .00001) {return $false}
    # Actual policy's largest possible dry density is dry*dry (force<=1).
    # Reserve margin below its .025 emission floor; do not demand storm=.95.
    $dry=(1-$soil.wetness)*(1-$weather.liquidRain)
    return $weather.liquidRain -gt .25 -and $soil.wetness -ge .3 -and $dry*$dry -lt .02
}
function Number([double]$value) {
    Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite fixture number.'
    return $value.ToString('R',$culture)
}
function Sea-Candidates([Nullable[double]]$x,[Nullable[double]]$z,[double]$originX,[double]$originZ) {
    Require (($null -eq $x) -eq ($null -eq $z)) 'Supply both SeaX and SeaZ, or neither.'
    if ($null -ne $x) {
        $null=Number ([double]$x);$null=Number ([double]$z)
        # PowerShell unwraps a singleton nested coordinate array; a named
        # record keeps both actual bound coordinates together through foreach.
        return [pscustomobject]@{x=[double]$x;z=[double]$z}
    }
    $null=Number $originX;$null=Number $originZ
    for($i=1;$i -le 10;++$i) {[pscustomobject]@{x=$originX-$i*128;z=$originZ}}
}
function Assert-RotorState($state) {
    foreach($field in @('timeMs','object','candidates','distanceXZ','x','y','z','groundY','clearance','rpm',
      'upX','upY','upZ','speedX','speedY','speedZ','fuel','damage','driverState','moveUp','moveDown','unfocusedMoveUp','focusLost','qKey','airflowScale','cameraDistance',
      'altKey','cameraType','moveForward','fastForward','moveBack','turnLeft','turnRight','moveLeft','moveRight','windX','windY','windZ')) {
        $property=$state.PSObject.Properties[$field]
        Require ($null -ne $property -and $null -ne $property.Value -and $property.Value -is [ValueType] -and $property.Value -isnot [bool]) ('Missing/nonnumeric rotor-state field '+$field)
        $value=[double]$property.Value
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) ('Nonfinite rotor-state field '+$field)
    }
    foreach($field in @('readonly','engineOn','destroyed','local','airborne','driverPresent','driverBrain','driverLocal','driverAlive','driverIsPlayer','pilotIsPlayer',
      'focusIsDriver','manual','driverManual','playerManual','playerSuspended','cameraEffect','resolvedHelicopter','airflowEnabled','cameraDistanceValid',
      'mouseTurnActive','lookAroundEnabled','lookAroundToggled','joystickActive','joystickThrustActive','hoverStateAvailable')) {
        Require ($state.PSObject.Properties[$field].Value -is [bool]) ('Missing/nonboolean rotor-state field '+$field)
    }
    foreach($field in @('class','model','inputContext','resolvedContext','inputSeat')) {
        Require ($state.PSObject.Properties[$field].Value -is [string]) ('Missing/nonstring rotor-state field '+$field)
    }
    Require ($state.readonly -and $state.class -ceq 'UH60' -and $state.candidates -ge 1 -and $state.distanceXZ -le 16.001 -and $state.rpm -ge 0 -and $state.rpm -le 1.001) 'Diagnostic does not identify the bounded actual stock helicopter.'
    if ($state.hoverStateAvailable) {
        foreach($field in @('hoveringAutopilot','pilotSpeedHelper','pilotHeightHelper','pilotDirectionHelper')) {
            Require ($state.PSObject.Properties[$field].Value -is [bool]) ('Missing actual hover flag '+$field)
        }
        foreach($field in @('pilotWantedX','pilotWantedY','pilotWantedZ','mouseWantedX','mouseWantedY','mouseWantedZ','pilotWantedHeight','pilotWantedHeading','pilotWantedDive')) {
            $v=$state.PSObject.Properties[$field].Value
            Require ($v -is [ValueType] -and $v -isnot [bool] -and ![double]::IsNaN([double]$v) -and ![double]::IsInfinity([double]$v)) ('Missing/nonfinite actual hover request '+$field)
        }
    }
    # Wrong authority/engine/input state is evidence, not a new acceptance gate.
}
function Rotor-CanopyClearance($state,[double]$top) {
    # Audited stock UH60 Geometry bounds, expanded 10% as Helicopter::Animate.
    # Rounded outward, allowing any sphere centre within the geometry bounds:
    # minY=-3.00, horizontal radius=13.20. Unit up direction
    # gives an orientation-safe vertical envelope for every yaw, including tilt.
    Require ($state.model -ieq 'data3d\uh-60.p3d') 'Unaudited helicopter model cannot use stock canopy envelope.'
    $length=[Math]::Sqrt($state.upX*$state.upX+$state.upY*$state.upY+$state.upZ*$state.upZ)
    Require ($length -ge .999 -and $length -le 1.001 -and $state.upY -gt 0 -and $state.upY -le 1.001) 'Actual aircraft rotation cannot establish canopy clearance.'
    $up=[Math]::Min(1.0,[double]$state.upY/$length)
    return [double]$state.y-3.00*$up-13.20*[Math]::Sqrt([Math]::Max(0.0,1.0-$up*$up))-$top
}
function Assert-NeutralHover($state) {
    Require ($state.hoverStateAvailable -and $state.hoveringAutopilot -and $state.playerManual -and !$state.playerSuspended -and !$state.cameraEffect -and $state.lookAroundEnabled) 'Manual canopy observation lacks actual unsuspended stock auto-hover.'
    foreach($field in @('moveUp','moveDown','moveForward','fastForward','moveBack','turnLeft','turnRight','moveLeft','moveRight','pilotWantedX','pilotWantedY','pilotWantedZ')) {
        Require ([Math]::Abs([double]$state.$field) -le .001) ('Canopy observation has active pilot request '+$field)
    }
}
$seaCandidates=@(Sea-Candidates $SeaX $SeaZ $SandX $SandZ)
if ($SelfTest) {
    $bound=@(Sea-Candidates $SeaX $SeaZ $SandX $SandZ)
    if ($null -ne $SeaX) {
        Require ($bound.Count -eq 1 -and $bound[0].x -is [double] -and $bound[0].z -is [double] -and $bound[0].x -eq [double]$SeaX -and $bound[0].z -eq [double]$SeaZ) 'Actual script SeaX/SeaZ binding lost a coordinate.'
    } else {Require ($bound.Count -eq 10 -and $bound[0].x -eq $SandX-128 -and $bound[-1].x -eq $SandX-1280 -and @($bound|Where-Object {$_.z -ne $SandZ}).Count -eq 0) 'Default read-only sea search changed.'}
    $single=@(Sea-Candidates 1750 3100 2475 5075)
    Require ($single.Count -eq 1 -and $single[0].x -eq 1750 -and $single[0].z -eq 3100) 'Singleton audited sea pair flattened.'
    $zero=@(Sea-Candidates 0 0 2475 5075);Require ($zero.Count -eq 1 -and $zero[0].x -eq 0 -and $zero[0].z -eq 0) 'Finite zero sea coordinate was treated as missing.'
    foreach($bad in @('missingX','missingZ','nanX','infZ')) {
        $refused=$false
        try {switch($bad) {'missingX'{$null=Sea-Candidates $null 3100 2475 5075};'missingZ'{$null=Sea-Candidates 1750 $null 2475 5075};'nanX'{$null=Sea-Candidates ([double]::NaN) 3100 2475 5075};'infZ'{$null=Sea-Candidates 1750 ([double]::PositiveInfinity) 2475 5075}}} catch {$refused=$true}
        Require $refused ('Malformed actual sea binding accepted: '+$bad)
    }
    $sample=[ordered]@{}
    foreach($field in @('timeMs','object','candidates','distanceXZ','x','y','z','groundY','clearance','rpm','upX','upY','upZ','speedX','speedY','speedZ','fuel','damage','driverState','moveUp','moveDown','unfocusedMoveUp','focusLost','qKey','airflowScale','cameraDistance','altKey','cameraType','moveForward','fastForward','moveBack','turnLeft','turnRight','moveLeft','moveRight','windX','windY','windZ')) {$sample[$field]=0.0}
    foreach($field in @('readonly','engineOn','destroyed','local','airborne','driverPresent','driverBrain','driverLocal','driverAlive','driverIsPlayer','pilotIsPlayer','focusIsDriver','manual','driverManual','playerManual','playerSuspended','cameraEffect','resolvedHelicopter','airflowEnabled','cameraDistanceValid','mouseTurnActive','lookAroundEnabled','lookAroundToggled','joystickActive','joystickThrustActive','hoverStateAvailable')) {$sample[$field]=$false}
    $sample.readonly=$true;$sample.class='UH60';$sample.model='uh60.p3d';$sample.inputContext='Spectator';$sample.resolvedContext='Spectator';$sample.inputSeat='driver';$sample.candidates=1
    Assert-RotorState ([pscustomobject]$sample)
    # A failed engine/authority snapshot must remain inspectable.
    foreach($bad in @('missing','string','nonfinite','readonly','class','rpm')) {
        $copy=($sample|ConvertTo-Json|ConvertFrom-Json)
        switch($bad) {'missing'{$copy.PSObject.Properties.Remove('qKey')};'string'{$copy.moveUp='false'};'nonfinite'{$copy.rpm=[double]::PositiveInfinity};'readonly'{$copy.readonly=$false};'class'{$copy.class='Other'};'rpm'{$copy.rpm=2}}
        $refused=$false;try {Assert-RotorState $copy} catch {$refused=$true};Require $refused ('Malformed snapshot accepted: '+$bad)
    }
    $geometry=[pscustomobject]@{model='data3d\uh-60.p3d';y=24.7666;upX=0;upY=1;upZ=0}
    Require ((Rotor-CanopyClearance $geometry 20.62515) -gt .35) 'Lowered actual-stock canopy safe envelope refused.'
    Require ((Rotor-CanopyClearance $geometry 23.62515) -lt 0) 'Original lifted canopy overlap missed.'
    $geometry.upX=.3;$geometry.upY=[Math]::Sqrt(1-.3*.3)
    Require ((Rotor-CanopyClearance $geometry 20.62515) -lt .35) 'Tilted underside excursion missed.'
    $hover=$sample|ConvertTo-Json|ConvertFrom-Json
    $hover.hoverStateAvailable=$true;$hover.playerManual=$true;$hover.lookAroundEnabled=$true
    foreach($field in @('hoveringAutopilot','pilotSpeedHelper','pilotHeightHelper','pilotDirectionHelper')) {$hover|Add-Member -NotePropertyName $field -NotePropertyValue $true}
    foreach($field in @('pilotWantedX','pilotWantedY','pilotWantedZ','mouseWantedX','mouseWantedY','mouseWantedZ','pilotWantedHeight','pilotWantedHeading','pilotWantedDive')) {$hover|Add-Member -NotePropertyName $field -NotePropertyValue 0.0}
    Assert-RotorState $hover;Assert-NeutralHover $hover
    $hover.pilotWantedZ=.1;$refused=$false;try {Assert-NeutralHover $hover} catch {$refused=$true};Require $refused 'Active pilot request accepted as neutral canopy observation.'
    Require ($canopyOffsets.Count -eq 5) 'Dense canopy count changed.'
    for($i=1;$i -lt $canopyOffsets.Count;++$i) {Require (($canopyOffsets[$i]-$canopyOffsets[$i-1]) -lt 4.78717685) 'Physical ViewGeometry roofs leave a known X gap.'}
    Require (6 -gt 4.78717685) 'Old 6m pitch must remain a source-gap negative control.'
    Require (Wet-Ready @{rain=.47823584;liquidRain=.47823584;particleSnowflakes=$false} @{wetness=.81652033}) 'Actual wet-ground suppression-ready forcing refused.'
    Require (!(Wet-Ready @{rain=.3;liquidRain=.3;particleSnowflakes=$false} @{wetness=.3})) 'Insufficient actual suppression forcing accepted.'
    Require (!(Wet-Ready @{rain=.9;liquidRain=.9;particleSnowflakes=$true} @{wetness=.9})) 'Snow accepted as natural liquid rain.'
    Write-Host 'PASS read-only rotor snapshot schema/falsifiers; failed flight authority remains evidence. No game.';return
}
if (!$env:LOCK_OWNER) {throw 'Run through scripts/with-game-lock.sh.'}
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) {throw 'Preserve the running game.'}
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
$result=[ordered]@{status='running';before=$before;gates=@{};captures=@{};rotorStates=@();
    limitations='Actual land emission/lifecycle gates only. Stills require visual review; this does not numerically accept pre-existing water downwash or all-map source coverage.'}
@{sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
  roofHelperSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
  installed=$before;mission=$mission;lockOwner=$env:LOCK_OWNER;requestedSand=@($SandX,$SandZ);
  helicopter='stock UH60';pilot='stock SoldierWB in independent west group';manualPilot=[bool]$ManualPilot;hoverHeight=$HoverHeight;drawTrace=[bool]$DrawTrace;
  drawTraceScope='Optional all-land-puffs CPU DrawDecal invocation/window; not backend acceptance, GPU pixels or exact screenshot-frame identity';
  observationSimulationSeconds=$ObserveSeconds;seaLevel=0;seaScope='Stock Noe default sea datum, bounded runtime terrain queries';
  relocation='One explicit setPosASL between stages; no setVelocity, forced RPM, repeated position pinning, synthetic particle or terrain stamps';
  diagnostics='Read-only rotor_state snapshots; unchanged initial flight recipe; actual timeMs captures query latency'} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES',
    'POSEIDON_SNOWLINE','POSEIDON_ROTOR_LAND','POSEIDON_ROTOR_LAND_TRACE','POSEIDON_ROTOR_LAND_DRAW_TRACE','POSEIDON_WIND_OVERRIDE',
    'WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_RAIN_WATER_TRACE')
$saved=@{};foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$writer=$null;$reader=$null;$script:heliId=$null;$script:stateObject=$null;$script:heldUp=$false;$script:heldLook=$false;$script:canopyTop=$null
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
function Rotor-State([string]$stage,[double]$x,[double]$z) {
    $state=Send @{cmd='rotor_state';x=$x;z=$z;radius=16}
    $entry=@{stage=$stage;wallUtc=[DateTime]::UtcNow.ToString('o');state=$state}
    $result.rotorStates+=,$entry
    $entry|ConvertTo-Json -Compress -Depth 6|Add-Content -LiteralPath (Join-Path $output 'rotor-states.jsonl')
    Assert-RotorState $state
    if ($null -eq $script:stateObject) {$script:stateObject=$state.object}
    Require ($state.object -eq $script:stateObject) 'Nearest diagnostic switched aircraft; retain raw snapshot and reject fixture identity.'
    return $state
}
function Current-RotorState([string]$stage) {
    $actual=Eval 'getPosASL rotorHeli';Require ($actual.Count -eq 3) 'Actual aircraft diagnostic locator missing.'
    return Rotor-State $stage ([double]$actual[0]) ([double]$actual[1])
}
function Capture([string]$stage) {
    if ($ManualPilot) {
        $direction=@(-30.0,-19.0,36.0);$length=[Math]::Sqrt(30*30+19*19+36*36)
        $raw=@(($script:manualViewX+30),($script:manualViewGround+27),($script:manualViewZ-36))+@($direction | ForEach-Object {$_/$length})
        Require ((Eval ('triSetView ['+(($raw | ForEach-Object {Number $_}) -join ',')+']')) -ceq 'OK') 'Paused render-only rotor view refused.'
    }
    $path=Join-Path $output ($stage+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    Require ((Get-Item -LiteralPath $path).Length -gt 100) 'Screenshot empty.'
    $result.captures[$stage]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
    if ($ManualPilot) {Require ((Eval 'triClearView') -ceq 'OK') 'Paused render-only rotor view did not release.'}
}
function Height([double]$x,[double]$z) {Get-TerrainPuddleFixtureHeight {param($request) Send $request} $x $z}
function View([double]$x,[double]$z,[double]$ground) {
    if ($ManualPilot) {
        $script:manualViewX=$x;$script:manualViewZ=$z;$script:manualViewGround=$ground
        Exec 'rotorHeli switchCamera "EXTERNAL"'
        Require ([double](Eval 'triGetCameraEffectActive') -eq 0) 'A camera effect suspends ordinary helicopter controls.'
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
    $poses=@();$nextState=$start;$beforeState=Current-RotorState ($stage+'-before')
    $proof=@{startTime=$start;rows=@();actualPositions=@();canopyChecks=@();status='observing'};$result.gates[$stage]=$proof
    if ($null -ne $script:canopyTop) {
        $gap=Rotor-CanopyClearance $beforeState $script:canopyTop;$proof.canopyChecks+=@{state=$beforeState;clearance=$gap;canopyTop=$script:canopyTop}
        if ($ManualPilot) {Assert-NeutralHover $beforeState}
        Require ($gap -ge .35) 'Stock roof overlaps conservative actual aircraft envelope before observation.'
    }
    Exec 'setAccTime 1'
    do {
        Start-Sleep -Milliseconds 250;Health;$position=Eval 'getPosASL rotorHeli'
        Require ($position.Count -eq 3 -and (Eval 'alive rotorHeli')) 'Aircraft lost during actual physics interval.'
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$x,2)+[Math]::Pow($position[1]-$z,2))
        $poses+=,@($position);$now=[double](Eval 'time')
        $proof.actualPositions=$poses;$proof.endTime=$now
        if ($now -ge $nextState -or $null -ne $script:canopyTop) {
            $state=Rotor-State ($stage+'-milestone') ([double]$position[0]) ([double]$position[1]);$nextState=$now+2
            if ($null -ne $script:canopyTop) {
                $gap=Rotor-CanopyClearance $state $script:canopyTop;$proof.canopyChecks+=@{state=$state;clearance=$gap;canopyTop=$script:canopyTop}
                if ($ManualPilot) {Assert-NeutralHover $state}
                Require ($gap -ge .35) 'Conservative tilted aircraft envelope approached canopy; fixture unavailable, not emission acceptance.'
            }
        }
        Require ($distance -le 8) 'Actual hover left the bounded source fixture; not an emission defect.'
        Require ([DateTime]::UtcNow -lt $until) 'Simulation observation timed out.'
    } while ($now-$start -lt $ObserveSeconds)
    Exec 'setAccTime 0';$null=Current-RotorState ($stage+'-after');Start-Sleep -Milliseconds 300;$rows=@(Rows $first)
    if ($mustTrace) {Require ($rows.Count -ge 3) 'No sustained actual rotor/RPM trace; fixture unavailable, not accepted.'}
    foreach($row in $rows) {Require ([Math]::Sqrt([Math]::Pow($row.x-$x,2)+[Math]::Pow($row.z-$z,2)) -le 8) 'Trace belongs outside requested fixture.'}
    $proof.endTime=$now;$proof.rows=$rows;$proof.actualPositions=$poses;$proof.engineOn=Eval 'isEngineOn rotorHeli';$proof.status='observed'
    Capture $stage;return $rows
}
function Place([double]$x,[double]$z,[double]$height) {
    $null=Current-RotorState ('place-'+(Number $height)+'-before')
    $ground=Height $x $z;$xz=(Number $x)+','+(Number $z)
    Exec ('rotorHeli setPosASL ['+$xz+','+(Number ($ground+$height))+']; rotorHeli flyInHeight '+(Number $height))
    $null=Rotor-State ('place-'+(Number $height)+'-relocated') $x $z
    if ($ManualPilot) {
        # Ordinary Q input refreshes the keyboard height helper after the one
        # stage relocation. Rotor RPM/velocity/physics remain untouched.
        $null=Send @{cmd='key';sc=20;hold=$true};$script:heldUp=$true
        $null=Current-RotorState ('place-'+(Number $height)+'-q-pressed-paused')
        Exec 'setAccTime 1';Start-Sleep -Milliseconds 250
        $null=Current-RotorState ('place-'+(Number $height)+'-q-held-before-release')
        $null=Send @{cmd='key_up';sc=20};$script:heldUp=$false;Exec 'setAccTime 0'
        $null=Current-RotorState ('place-'+(Number $height)+'-q-released-paused')
    } else {Exec ('rotorPilot doMove ['+$xz+',0]')}
    View $x $z $ground;$null=Current-RotorState ('place-'+(Number $height)+'-after-view');return $ground
}
function AwaitGroundedSpool {
    # Actual diagnostics proved the prior relocation dropped a stopped rotor
    # from 18 m and destroyed the airframe. Let its real engine spool on ground.
    $start=Current-RotorState 'grounded-spool-before'
    Require (!$start.destroyed -and $start.damage -eq 0 -and $start.clearance -lt 6 -and $start.engineOn) 'Grounded undamaged engine-on spool fixture missing.'
    $until=[DateTime]::UtcNow.AddSeconds(35);Exec 'setAccTime 1'
    do {
        Start-Sleep -Milliseconds 500;$state=Current-RotorState 'grounded-spool-poll'
        Require (!$state.destroyed -and $state.damage -eq 0 -and $state.engineOn -and $state.clearance -lt 6) 'Real ground spool lost engine or undamaged ground support.'
        Require ([DateTime]::UtcNow -lt $until) 'Actual grounded rotor failed to reach operating RPM.'
    } while ($state.rpm -lt .85)
    Exec 'setAccTime 0';$result.gates.groundedSpool=@{before=$start;after=$state;elapsedMs=$state.timeMs-$start.timeMs}
}
function AwaitHover([double]$x,[double]$z) {
    $first=LineCount;$until=[DateTime]::UtcNow.AddSeconds(60);Exec 'setAccTime 1'
    $result.gates.hoverWait=@()
    do {
        Start-Sleep -Milliseconds 500;Health
        $position=Eval 'getPosASL rotorHeli'
        $snapshot=Rotor-State 'await-hover-poll' ([double]$position[0]) ([double]$position[1])
        $result.gates.hoverWait+=@{time=(Eval 'time');position=$position;engineOn=(Eval 'isEngineOn rotorHeli');rotorState=$snapshot}
        $rows=@(Rows $first | Where-Object {
            [Math]::Abs($_.x-$x) -le 8 -and [Math]::Abs($_.z-$z) -le 8 -and $_.height -ge 8 -and $_.height -lt 28 -and $_.rpm -ge .65})
        Require ([DateTime]::UtcNow -lt $until) 'Stock UH60 did not establish an actual bounded hover; no synthetic pinning fallback.'
    } while ($rows.Count -lt 2)
    $script:heliId=$rows[-1].object;Exec 'setAccTime 0';$null=Current-RotorState 'await-hover-completed';return $rows[-1]
}
function CloseOwned {
    if ($script:heldLook) {$null=Send @{cmd='key_up';sc=226};$script:heldLook=$false}
    if ($script:heldUp) {$null=Send @{cmd='key_up';sc=20};$script:heldUp=$false}
    Require ($writer -and $reader) 'Harness not connected for normal shutdown.'
    Exec 'setAccTime 1';$null=Send @{cmd='exit'}
    Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game did not quit normally with exit zero.'
    Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Normal shutdown proof missing.'
    $result.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys) {Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
    if ($DrawTrace) {$env:POSEIDON_ROTOR_LAND_DRAW_TRACE='1'}
    $env:POSEIDON_WIND_OVERRIDE='0 90 0'
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
    $crewSetup=if($ManualPilot){'rotorPilot=player; rotorPilot allowDamage false; '}else{'rotorGroup=createGroup west; "SoldierWB" createUnit [['+$xz+',0],rotorGroup,"rotorPilot=this; this allowDamage false; this setBehaviour ""CARELESS""; this setCombatMode ""BLUE"""]; '}
    Exec ('rotorHeli="UH60" createVehicle ['+$xz+',0]; rotorHeli allowDamage false; rotorHeli setFuel 0; rotorHeli engineOn false; '+$crewSetup+'rotorPilot moveInDriver rotorHeli; rotorHeli setPos ['+$xz+',0]; doStop rotorPilot; rotorHeli engineOn false')
    Require ((Eval 'typeOf rotorHeli') -ceq 'UH60' -and (Eval 'typeOf driver rotorHeli') -ceq 'SoldierWB') 'Real stock helicopter/crew identity missing.'
    $null=Rotor-State 'startup-boarded-engine-off' $SandX $SandZ
    if ($ManualPilot) {
        # Original OFP scripting has no selectPlayer command. Board the actual
        # mission player rather than trying to replace player authority.
        Exec 'rotorPilot action ["AUTOHOVER",rotorHeli]; rotorHeli switchCamera "EXTERNAL"'
        Require ((Eval 'format ["%1",player]') -ceq (Eval 'format ["%1",rotorPilot]')) 'Actual controlled pilot identity differs.'
    }
    $result.gates.object=Eval 'format ["%1",rotorHeli]';View $SandX $SandZ $ground
    $null=Current-RotorState 'startup-after-view'
    $rows=@(Observe 'engine-off' $SandX $SandZ $false);Require (!$result.gates['engine-off'].engineOn -and $rows.Count -eq 0) 'Stopped rotor emitted land puffs.'
    $null=Current-RotorState 'before-engine-start'
    Exec 'rotorHeli setFuel 1; rotorHeli engineOn true';$null=Current-RotorState 'engine-start-paused'
    AwaitGroundedSpool
    if ($ManualPilot) {
        # Ordinary look-around prevents an external-view mouse direction from
        # requesting reverse speed during stock auto-hover; physics is untouched.
        $null=Send @{cmd='key';sc=226;hold=$true};$script:heldLook=$true
    }
    $ground=Place $SandX $SandZ $HoverHeight
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
    # Twenty-five audited stock roofs form an overlapping physical ViewGeometry
    # canopy. Source slanted-ray probes justify this candidate, not acceptance.
    # Acceptance still requires actual physical hit counts, not this layout guess.
    Exec 'rotorRoofs=[]';$roofProof=@()
    # A shared centre-ground datum keeps hillside tents from intersecting the
    # naturally settled aircraft. Buried outer walls are test-canopy geometry,
    # not a proposed placement change for ordinary map objects.
    $canopyGround=Height $SandX $SandZ;$canopyOrigin=$canopyGround+2.8100528717041+.02
    $canopyTop=$canopyOrigin+2.8100528717041
    foreach($dx in $canopyOffsets) {foreach($dz in $canopyOffsets) {
        $roof=New-TerrainPuddleStockRoof -SendCommand {param($request) Send $request} -X ($SandX+$dx) -Z ($SandZ+$dz) -RoofLift 0
        $roof.initialPosition=$roof.actualPosition
        Exec ('puddleStockRoof setPosASL ['+(Number ($SandX+$dx))+','+(Number ($SandZ+$dz))+','+(Number $canopyOrigin)+']')
        $actual=Eval 'getPosASL puddleStockRoof'
        Require ($actual.Count -eq 3 -and [Math]::Abs($actual[0]-($SandX+$dx)) -le .01 -and [Math]::Abs($actual[1]-($SandZ+$dz)) -le .01 -and [Math]::Abs($actual[2]-$canopyOrigin) -le .01) 'Actual stock canopy shared-datum relocation differs.'
        $roof.actualPosition=$actual;$roof.commonDatumGround=$canopyGround;$roof.actualTop=[double]$actual[2]+2.8100528717041
        Exec 'rotorRoofs=rotorRoofs+[puddleStockRoof]';$roofProof+=,$roof
    }}
    $result.gates.roofObjects=$roofProof;$script:canopyTop=($roofProof|Measure-Object actualTop -Maximum).Maximum
    $result.gates.canopyGeometry=@{origin=$canopyOrigin;centreGround=$canopyGround;expectedTop=$canopyTop;actualMaxTop=$script:canopyTop;heliModel='data3d\uh-60.p3d';modelMinY=-2.44864;envelopeMinY=-3.00;envelopeHorizontalRadius=13.20;requiredClearance=.35;pitch=4;offsets=$canopyOffsets;physicalViewRoofWidth=4.78717685;viewGeometryLevel=5;scope='Bounded overlapping physical-ViewGeometry test canopy; outer lower walls may be buried'}
    $rows=@(Observe 'roof-covered' $SandX $SandZ)
    Require (@($rows | Where-Object {$_.sheltered -ne 8}).Count -eq 0) 'Roof fixture did not block all eight actual bounded airflow probes in every recorded pulse.'
    Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -eq 0) 'Covered rotor spokes still emitted; retain actual rays and inspect canopy coverage.'
    Exec '{deleteVehicle _x} forEach rotorRoofs';$script:canopyTop=$null
    Exec 'rotorRemovedCount=0; {if (isNull _x) then {rotorRemovedCount=rotorRemovedCount+1}} forEach rotorRoofs'
    Require ((Eval 'rotorRemovedCount') -eq 25) 'Actual test canopy did not leave the mission.'
    $rows=@(Observe 'roof-removed' $SandX $SandZ)
    Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -gt 0) 'Removed-roof dust positive control failed.'
    # Elevated active rotor must not emit land puffs. One stage relocation only.
    $ground=Place $SandX $SandZ 48;Exec 'setAccTime 1';Start-Sleep -Seconds 4;Exec 'setAccTime 0'
    $position=Eval 'getPosASL rotorHeli';Require ($position[2]-(Height $position[0] $position[1]) -gt 35) 'High-altitude fixture not actually high.'
    $rows=@(Observe 'high-altitude' $SandX $SandZ $false);Require ($rows.Count -eq 0 -and $result.gates['high-altitude'].engineOn) 'High active rotor emitted land puffs.'
    foreach($pose in $result.gates['high-altitude'].actualPositions) {Require ($pose[2]-(Height $pose[0] $pose[1]) -gt 35) 'Aircraft fell into the land emission window during high-altitude control.'}
    $ground=Place $SandX $SandZ $HoverHeight;$null=AwaitHover $SandX $SandZ
    Exec '0 setOvercast 1; 0 setRain 1; setAccTime 3';$until=[DateTime]::UtcNow.AddSeconds(90)
    do {Start-Sleep -Milliseconds 400;$weather=Send @{cmd='weather_visibility'};$soil=Send @{cmd='dev_sand';action='state'};Require ([DateTime]::UtcNow -lt $until) 'Real rain failed to establish actual wet-ground dust suppression.'} while (!(Wet-Ready $weather $soil))
    Exec 'setAccTime 0';$result.gates.realWet=@{weather=$weather;soil=$soil;water=(Send @{cmd='dev_rain_water';action='sample';x=$SandX;z=$SandZ})}
    $rows=@(Observe 'real-rain-wet' $SandX $SandZ);Require (@($rows | Where-Object {$_.emitted -gt 0}).Count -eq 0) 'Wet actual rainy ground emitted dry dust.'
    # Read-only shallow-sea search; no water material/control setter is used.
    $sea=$null
    foreach($candidate in $seaCandidates) {
        $bath=Send @{cmd='water_bathymetry';x=$candidate.x;z=$candidate.z}
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
