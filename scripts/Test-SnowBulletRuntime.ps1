# Actual ordinary-player shots, not a synthetic Shot or direct deficit stamp.
[CmdletBinding()]
param([switch]$SelfTest,[switch]$Refill,[switch]$AutoExposure,
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-snow-shot',
      [double]$PlayerX=4975,[double]$PlayerZ=4672.5,
      [ValidateRange(1,1000)][int]$AimDy=250,
      [ValidateRange(.08,.5)][double]$Depth=.18,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) {if (!$condition) {throw $message}}
function Decode-Eval([string]$display) {
    $display=$display.Trim()
    if ($display.StartsWith('"')) {return $display.Substring(1,$display.Length-2).Replace('""','"')}
    if ($display.StartsWith('[')) {
        # Preserve array shape on both PS5.1 (no NoEnumerate switch) and PS7.
        $wrapped=ConvertFrom-Json -InputObject ('{"value":'+$display+'}');return ,$wrapped.value
    }
    if ($display -cmatch '^(true|false|null|-?\d+(\.\d+)?([eE][+-]?\d+)?)$') {return ConvertFrom-Json -InputObject $display}
    return $display
}
function Number([double]$value) {
    Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite fixture coordinate.'
    return $value.ToString('R',$culture)
}
function Restore-Environment([string]$key,$value) {
    # PowerShell7.6/.NET10 can preserve an empty value when null is bound to SetEnvironmentVariable.
    if ($null -eq $value) {Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    else {[Environment]::SetEnvironmentVariable($key,$value,'Process')}
}
function Set-ExposurePolicy([bool]$automatic) {
    # These two overrides independently disable automatic time-of-day grading.
    Restore-Environment 'WGR_TONEMAP' $null;Restore-Environment 'WGR_HDR_ENCODE' $null
    Restore-Environment 'WGR_TONEMAP_WHITE' $null;Restore-Environment 'WGR_TONEMAP_DESAT' $null
    if ($automatic) {
        Restore-Environment 'WGR_EXPOSURE' $null;Restore-Environment 'WGR_AUTO_EXPOSURE' $null
        return 'normal-time-of-day-grade-and-default-adaptive-policy'
    }
    $env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='1'
    return 'legacy-fixed-manual-exposure1-adaptive-off; highlight-headroom-not-guaranteed'
}
function Assert-ShotSettled([long]$releaseMs,[long]$pausedMs) {
    Require ($releaseMs -ge 0 -and $pausedMs-$releaseMs -ge 2000) 'Shot image precedes two real simulation seconds after mouse-up.'
}
function Parse-SnowHit([string]$line) {
    $n='[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?'
    $pattern='SNOW_BULLET terrain=1 changed=(true|false) x=('+ $n+') y=('+ $n+') z=('+ $n+') speed=('+ $n+') base=('+ $n+') remaining=('+ $n+') radius=('+ $n+') depth=('+ $n+') before=('+ $n+') after=('+ $n+') roadway=(true|false) sheltered=(true|false) land=(true|false) budget=(true|false) up=('+ $n+') incidence=('+ $n+') supportError=('+ $n+')\s*$'
    Require ($line -cmatch $pattern) 'Missing or unknown actual terminal snow-impact trace.'
    $m=$Matches.Clone();$r=@{line=$line;changed=$m[1] -ceq 'true';roadway=$m[12] -ceq 'true';sheltered=$m[13] -ceq 'true';land=$m[14] -ceq 'true';budget=$m[15] -ceq 'true'}
    $keys=@('x','y','z','speed','base','remaining','radius','depth','before','after','up','incidence','supportError')
    $lanes=@(2,3,4,5,6,7,8,9,10,11,16,17,18)
    for($i=0;$i -lt $keys.Count;++$i) {
        $value=[double]::Parse($m[$lanes[$i]],$culture)
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite actual shot trace.';$r[$keys[$i]]=$value
    }
    Require ([Math]::Abs($r.x) -lt 1000000 -and [Math]::Abs($r.z) -lt 1000000 -and [Math]::Abs($r.y) -lt 1000000) 'Unbounded actual impact position.'
    Require ($r.speed -ge 30 -and $r.speed -le 5000 -and $r.up -ge .5 -and $r.up -le 1.001 -and $r.incidence -le -.04 -and $r.incidence -ge -1.001 -and [Math]::Abs($r.supportError) -le .0801) 'Actual shot did not establish the bounded downward terrain fixture.'
    Require ($r.base -ge 0 -and $r.remaining -ge 0 -and $r.remaining -le $r.base+.0001 -and $r.radius -ge 0 -and $r.radius -le .3501 -and $r.depth -ge 0 -and $r.depth -le .1601 -and $r.before -ge 0 -and $r.after -ge 0 -and $r.after -le $r.base+.0001) 'Actual snow cut exceeds source bounds.'
    if ($r.changed) {Require ($r.land -and $r.budget -and !$r.roadway -and !$r.sheltered -and $r.after -gt $r.before) 'Changed cut lacks actual source admission.'}
    return $r
}
function Cell-Key([int]$x,[int]$z) {return "$x`:$z"}
function New-HoleViews([double]$x,[double]$z,[double]$ground,[double]$snowDepth) {
    return [ordered]@{
        low=@{position=@(($x-1.4),($ground+$snowDepth+.5),($z-1.4));target=@($x,($ground+$snowDepth-.03),$z)}
        top=@{position=@($x,($ground+$snowDepth+2.2),($z-.03));target=@($x,($ground+$snowDepth-.03),$z)}
    }
}
function Assert-Hole($hit,$before,$after,[double]$base) {
    Require ($hit.changed -and $hit.after-$hit.before -gt .001) 'Actual terminal shot did not make a stored hole.'
    $changed=@();$matched=@();$outer=0
    foreach($key in $after.Keys) {
        Require ($before.Contains($key)) 'Before/after cell inventory differs.'
        $b=$before[$key];$a=$after[$key];$distance=[Math]::Sqrt([Math]::Pow($a.x-$hit.x,2)+[Math]::Pow($a.z-$hit.z,2))
        Require ($a.deficit -ge 0 -and $a.deficit -le $base+.0001) 'Actual sampled cut exceeds local snow depth.'
        $delta=$a.deficit-$b.deficit
        Require ($delta -ge -.00001) 'Nonfalling snow unexpectedly refilled during the shot.'
        if ($delta -gt .00001) {Require ($distance -le $hit.radius+.09) 'Actual changed cell escaped impact kernel.';$changed+=,$a}
        if ($distance -gt $hit.radius+.13) {Require ([Math]::Abs($delta) -lt .00001) 'Outside powder changed.';++$outer}
        # World trace is rounded to 1 mm; near a cell boundary, either adjoining
        # cell can be selected by the displayed coordinate. Match the actual
        # before/after values on a centre within half the cell diagonal.
        if ($distance -le .09 -and [Math]::Abs($a.deficit-$hit.after) -le .00011 -and [Math]::Abs($b.deficit-$hit.before) -le .00011) {$matched+=,$a}
    }
    Require ($changed.Count -gt 0 -and $matched.Count -gt 0 -and $outer -gt 0) 'No actual impact-centre/outside correspondence.'
    $peak=($changed | ForEach-Object {$_.deficit} | Measure-Object -Maximum).Maximum
    $rim=@($changed | Where-Object {
        [Math]::Sqrt([Math]::Pow($_.x-$hit.x,2)+[Math]::Pow($_.z-$hit.z,2)) -gt $hit.radius*.5 -and $_.deficit -lt $peak-.0001})
    Require ($rim.Count -gt 0) 'Actual sampled crater has no shallower radial edge.'
    return @{changedCells=$changed;centre=$matched[0];rimCells=$rim;unchangedOutsideCells=$outer;maximumDeficit=$peak}
}
if ($SelfTest) {
    $sample='[info] SNOW_BULLET terrain=1 changed=true x=4975.063 y=20.000 z=4675.063 speed=850.00 base=0.1800 remaining=0.1800 radius=0.2250 depth=0.0840 before=0.0000 after=0.0840 roadway=false sheltered=false land=true budget=true up=1.0000 incidence=-0.7500 supportError=0.0000'
    $hit=Parse-SnowHit $sample
    foreach($bad in @($sample.Replace('after=0.0840','after=0.2000'),$sample.Replace('radius=0.2250','radius=0.4000'),$sample.Replace('roadway=false','roadway=true'),$sample.Replace('speed=850.00','speed=NaN'),$sample.Replace('speed=850.00','speed=1e309'),$sample.Replace('incidence=-0.7500','incidence=0.1000'),($sample+' unknown=1'))) {
        $failed=$false;try {$null=Parse-SnowHit $bad} catch {$failed=$true};Require $failed 'Malformed/unsupported trace accepted.'
    }
    $views=New-HoleViews 10 20 30 .18;Require ($views.low.position.Count -eq 3 -and [Math]::Abs($views.low.position[1]-30.68) -lt .00001 -and [Math]::Abs($views.top.target[1]-30.15) -lt .00001) 'Engine XYZ camera arithmetic failed.'
    $b=@{};$a=@{};for($iz=-3;$iz -le 3;++$iz) {for($ix=-3;$ix -le 3;++$ix) {
        $key=Cell-Key $ix $iz;$x=$hit.x+$ix*.125;$z=$hit.z+$iz*.125;$value=if($ix -eq 0 -and $iz -eq 0){.084}elseif([Math]::Abs($ix)+[Math]::Abs($iz) -eq 1){.035}else{0}
        $b[$key]=@{x=$x;z=$z;deficit=0};$a[$key]=@{x=$x;z=$z;deficit=$value}
    }}
    $null=Assert-Hole $hit $b $a .18;$key=Cell-Key 3 3;$a[$key].deficit=.01
    $failed=$false;try {$null=Assert-Hole $hit $b $a .18} catch {$failed=$true};Require $failed 'Outside mutation accepted.'
    Require ((Decode-Eval 'WEST Alpha:1 (mail)') -ceq 'WEST Alpha:1 (mail)' -and (Decode-Eval '[1,2,3]').Count -eq 3) 'SQF decoder failed.'
    Require ((Decode-Eval '[1]').Count -eq 1 -and (Decode-Eval '[]').Count -eq 0 -and (Decode-Eval 'true') -is [bool]) 'Array/scalar SQF types changed.'
    Assert-ShotSettled 10000 12000
    foreach($times in @(@(10000,11999),@(-1,2000),@(10000,9000))) {
        $failed=$false;try {Assert-ShotSettled $times[0] $times[1]} catch {$failed=$true};Require $failed 'Unsettled shot accepted.'
    }
    $envKeys=@('WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_TONEMAP_WHITE','WGR_TONEMAP_DESAT');$prior=@{}
    foreach($key in $envKeys) {$prior[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
    try {
        foreach($key in $envKeys) {[Environment]::SetEnvironmentVariable($key,'1','Process')}
        Require ((Set-ExposurePolicy $true) -ceq 'normal-time-of-day-grade-and-default-adaptive-policy') 'Normal exposure mode mislabeled.'
        foreach($key in $envKeys) {Require ($null -eq [Environment]::GetEnvironmentVariable($key,'Process')) 'Normal mode retained a grading/adaptive pin.'}
        $null=Set-ExposurePolicy $false;Require ($env:WGR_EXPOSURE -ceq '1' -and $env:WGR_AUTO_EXPOSURE -ceq '0') 'Fixed legacy arm changed.'
        Restore-Environment 'WGR_EXPOSURE' $null;Require ($null -eq [Environment]::GetEnvironmentVariable('WGR_EXPOSURE','Process')) 'Previously absent environment variable was retained.'
    } finally {foreach($key in $envKeys) {Restore-Environment $key $prior[$key]}}
    Write-Host 'PASS actual shot trace, kernel-boundary falsifier, camera/decode, two-second settling, exposure policy and null-safe restoration checks (no game/GPU/profile).';return
}
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ($stamp.Length -gt 0) 'Missing installed provenance.'
    return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$before=Pair
$output=Join-Path $root ('build/snow-bullet/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $output 'user';New-Item -ItemType Directory -Force $profile | Out-Null
$log=Join-Path $output 'engine.log';$mission=Join-Path $root 'tests/perf/missions/perf_sand.noe'
$result=[ordered]@{status='running';before=$before;stages=@{};captures=@{};
    autoExposure=[bool]$AutoExposure;brightness=1;minimumSettleSimulationMs=2000;
    limitations=@('Source collision remains bare terrain.','Rim is uncut surrounding powder, not newly heaped mass.','Hard-object/wall/sea branches omit SNOW_BULLET terminal logs; those require a future actual ballistic-terminal query and are not accepted by absence.','Pictures require visual review; source/cell counters do not accept hole appearance.','Two seconds of real simulation reduce transient impact FX; authored persistent decals remain. No effects are deleted.','AutoExposure restores normal time-of-day grading and default adaptive policy, not a forced adaptive exposure enable. Fixed exposure1 controls can still clip snow.')}
@{sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
  installed=$before;lockOwner=$env:LOCK_OWNER;mission=$mission;player='SoldierWB';weapon='M16';requestedPlayer=@($PlayerX,$PlayerZ);
  aim='Ordinary relative SDL mouse motion';aimDy=$AimDy;depth=$Depth;refill=[bool]$Refill;autoExposure=[bool]$AutoExposure;brightness=1;
  firing='SDL mouse button1; Fired event records only; terminal impact + at least2000sim ms after mouse-up before pause';gridSpacing=.125;captureVectors='raw engine XYZ'} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN',
    'POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','POSEIDON_SNOW_BULLET_TRACE',
    'WGR_TERRAIN_PUDDLE_WETNESS','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_TONEMAP_WHITE','WGR_TONEMAP_DESAT','WGR_TEMPORAL','WGR_GRASS','POSEIDON_WIND_OVERRIDE')
$saved=@{};foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$held=$false
function Health {
    Require ($p -and !$p.HasExited) 'Owned game exited unexpectedly.'
    if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot' -Quiet)) {throw 'Installed validation/script failure.'}
}
function Send($command) {
    Health;$line=$command | ConvertTo-Json -Compress;$line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$writer.WriteLine($line)
    do {$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$reply=$replyLine | ConvertFrom-Json} while ($null -eq $reply.ok)
    Require ($reply.ok -eq $true) $replyLine;return $reply
}
function Exec([string]$code) {$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code) {return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Clock {$state=Send @{cmd='query';what='play_state'};Require ($state.has_player -and $state.player_active -and $state.player_local) 'Actual local controlled player missing.';return [long]$state.time_ms}
function HitLines {return @(Select-String -LiteralPath $log -SimpleMatch 'SNOW_BULLET terrain=1' -ErrorAction SilentlyContinue)}
function Patch([double]$x,[double]$z) {
    $patch=@{};$cx=[int][Math]::Floor($x/.125);$cz=[int][Math]::Floor($z/.125)
    for($j=-6;$j -le 6;++$j) {for($i=-6;$i -le 6;++$i) {
        $ix=$cx+$i;$iz=$cz+$j;$px=($ix+.5)*.125;$pz=($iz+.5)*.125
        $state=Send @{cmd='dev_snow';action='sample';x=$px;z=$pz};$value=[double]$state.deficit
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value) -and $value -ge 0) 'Actual snow cell malformed.'
        $patch[(Cell-Key $ix $iz)]=@{x=$px;z=$pz;deficit=$value}
    }};return $patch
}
function Compare-Patches($a,$b,[string]$message) {
    Require ($a.Count -eq $b.Count) $message
    foreach($key in $a.Keys) {Require ($b.Contains($key) -and [Math]::Abs($a[$key].deficit-$b[$key].deficit) -lt .00001) $message}
}
function View($view) {
    $dir=@(($view.target[0]-$view.position[0]),($view.target[1]-$view.position[1]),($view.target[2]-$view.position[2]))
    $data=@($view.position)+@($dir);$code='triSetView ['+(($data | ForEach-Object {Number $_}) -join ',')+']'
    Require ((Eval $code) -ceq 'OK') 'Exact render camera refused.'
}
function Unview {Require ((Eval 'triClearView') -ceq 'OK') 'Render-only view did not release.'}
function Capture([string]$name) {
    Start-Sleep -Milliseconds 500;$time=Clock;$path=Join-Path $output ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    Require ((Clock) -eq $time) 'Paused simulation advanced during screenshot.'
    $bytes=[IO.File]::ReadAllBytes($path);Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Screenshot is not PNG.'
    $result.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;timeMs=$time;bytes=$bytes.Length;exposureMode=$result.exposureMode;brightness=1}
}
function Capture-Views([string]$stage,$views) {foreach($entry in $views.GetEnumerator()) {View $entry.Value;Capture ($stage+'-'+$entry.Key);Unview}}
function Shoot([string]$stage) {
    Unview;Exec 'player switchCamera "INTERNAL"'
    $ammo=[double](Eval 'player ammo "M16"');$shots=[int](Eval 'snowShotCount');$hits=@(HitLines).Count
    Require ($ammo -gt 0 -and [double](Eval 'triPlayerCurrentMagazineAmmo') -eq $ammo) 'Actual loaded selected M16 ammunition missing.'
    Exec 'setAccTime 0.05';$null=Send @{cmd='mouse_button';button=1;down=$true};$script:held=$true
    $until=[DateTime]::UtcNow.AddSeconds(20)
    do {Start-Sleep -Milliseconds 30;$now=[int](Eval 'snowShotCount');Require ([DateTime]::UtcNow -lt $until) 'Ordinary click produced no actual Fired event.'} while ($now -eq $shots)
    $null=Send @{cmd='mouse_button';button=1;down=$false};$script:held=$false;$releaseMs=Clock;Exec 'setAccTime 1'
    $shotTime=[double](Eval 'snowShotTime');$until=[DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 40;$lines=@(HitLines);$clock=Clock
        Require ([DateTime]::UtcNow -lt $until) 'Fired round produced no actual downward terminal terrain event; aim/fixture is unavailable.'
    } while ($lines.Count -le $hits -or $clock -lt $releaseMs+2000)
    Exec 'setAccTime 0';$pausedMs=Clock;Assert-ShotSettled $releaseMs $pausedMs
    $lines=@(HitLines);$now=[int](Eval 'snowShotCount');$afterAmmo=[double](Eval 'player ammo "M16"')
    Require ($now -eq $shots+1 -and $afterAmmo -eq $ammo-1 -and $lines.Count -eq $hits+1) 'Single actual bullet/Fired/ammunition/terrain-event proof differs.'
    $event=Eval 'snowShotSource';Require ($event.Count -eq 4 -and $event[0] -ceq 'M16' -and ![string]::IsNullOrWhiteSpace($event[3])) 'Actual Fired source weapon/ammo missing.'
    $hit=Parse-SnowHit $lines[-1].Line;$position=Eval 'getPosASL player'
    $distance=[Math]::Sqrt([Math]::Pow($hit.x-$position[0],2)+[Math]::Pow($hit.z-$position[1],2))
    Require ($distance -ge 1.4 -and $distance -le 8) 'Actual terminal hit is too close to boot edits or outside the bounded aiming patch.'
    $result.stages[$stage]=@{hit=$hit;ammoBefore=$ammo;ammoAfter=$afterAmmo;firedBefore=$shots;firedAfter=$now;eventSource=$event;firedTime=$shotTime;mouseUpTimeMs=$releaseMs;pausedTimeMs=$pausedMs;settleSimulationMs=($pausedMs-$releaseMs);playerPosition=$position;horizontalHitDistance=$distance;input='SDL left click; Fired handler did not pause; actual projectile terminal event + two seconds of simulation after release required'}
    return $hit
}
function CloseOwned {
    if ($held) {$null=Send @{cmd='mouse_button';button=1;down=$false};$script:held=$false}
    Exec 'setAccTime 1';$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit zero.'
    Require (Select-String -LiteralPath $log -SimpleMatch 'Shutdown complete' -Quiet) 'Normal shutdown proof missing.';$result.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys) {Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
    $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_SNOW_BULLET_TRACE='1';$env:POSEIDON_SNOWLINE='off';$env:POSEIDON_SNOW_TEST_DEPTH='0'
    $result.exposureMode=Set-ExposurePolicy ([bool]$AutoExposure);$env:WGR_TEMPORAL='0';$env:WGR_GRASS='0';$env:POSEIDON_WIND_OVERRIDE='0 90 0'
    $result.exposureEnvironment=@{};foreach($key in @('WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_TONEMAP_WHITE','WGR_TONEMAP_DESAT')) {$result.exposureEnvironment[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"version=3;`nqualityPreset=3;`nbrightness=1;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt');$null=$p.Handle
    $until=[DateTime]::UtcNow.AddSeconds(120)
    do {Health;$client=[Net.Sockets.TcpClient]::new();try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null};if (!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}} while (!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK' -and (Eval 'typeOf player') -ceq 'SoldierWB') 'Actual standard US player mission not ready.'
    Require ((Eval 'triSetBrightness 1') -ceq 'OK') 'Controlled brightness refused.'
    Require ((Eval 'triCheatInfiniteAmmo false') -ceq 'OK' -and [double](Eval 'triCheatInfiniteAmmoActive') -eq 0) 'Infinite ammo must be disabled.'
    Exec ('player allowDamage false; 0 setRain 0; 0 setFog 0; 0 setOvercast 0; removeAllWeapons player; player addMagazine "M16"; player addWeapon "M16"; player selectWeapon "M16"; player setUnitPos "UP"; player setPos ['+(Number $PlayerX)+','+(Number $PlayerZ)+',0]; player setDir 0; player switchCamera "INTERNAL"; snowShotCount=0; snowShotTime=-1; snowShotSource=[]; player addEventHandler ["Fired",{snowShotCount=snowShotCount+1; snowShotTime=time; snowShotSource=[_this select 1,_this select 2,_this select 3,_this select 4]}]; hint ""; setAccTime 1')
    $null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    Start-Sleep -Seconds 2;$null=Send @{cmd='mouse_motion';dx=0;dy=$AimDy};Start-Sleep -Milliseconds 700;Exec 'setAccTime 0'
    $state=Send @{cmd='dev_snow';action='state'};Require ($state.enabled -and !$state.falling -and $state.geometry -and $state.depth -eq 0) 'Enabled zero-depth dry calibration state missing.'
    $dry=Shoot 'no-snow-calibration';Require (!$dry.changed -and $dry.base -eq 0 -and $dry.before -eq 0 -and $dry.after -eq 0) 'Snow-absent real bullet created a cut.'
    $ground=Get-TerrainPuddleFixtureHeight ${function:Send} $dry.x $dry.z
    $views=New-HoleViews $dry.x $dry.z $ground $Depth;$result.views=$views
    $null=Send @{cmd='dev_snow';action='deposit';metres=$Depth};$state=Send @{cmd='dev_snow';action='state'}
    Require ($state.enabled -and !$state.falling -and $state.geometry -and [Math]::Abs($state.depth-$Depth) -lt .00001) 'Actual fixed deposited snow differs.'
    $beforePatch=Patch $dry.x $dry.z;$result.beforePatch=$beforePatch;Capture-Views 'deposited-before' $views
    $positive=Shoot 'snow-positive';Require ([Math]::Sqrt([Math]::Pow($positive.x-$dry.x,2)+[Math]::Pow($positive.z-$dry.z,2)) -le .25) 'Ordinary aiming/recoil moved the actual positive outside the calibrated before patch.'
    $afterPatch=Patch $dry.x $dry.z;$result.stages['snow-positive'].shape=Assert-Hole $positive $beforePatch $afterPatch $Depth;$result.afterPatch=$afterPatch
    Capture-Views 'hole-after' $views;$time=Clock;Start-Sleep -Seconds 5
    Require ((Clock) -eq $time) 'Paused snow-hole clock advanced.';Compare-Patches $afterPatch (Patch $dry.x $dry.z) 'Paused snow-hole cells changed.'
    $far=New-HoleViews ($dry.x+200) ($dry.z+200) $ground $Depth;View $far.top;Capture 'camera-away';Unview;Capture-Views 'camera-return' $views
    Compare-Patches $afterPatch (Patch $dry.x $dry.z) 'Camera return cleared or changed stored snow holes.'
    # Keep the real tent's wall hull above the downward projectile path.
    # At 3 m lift the covered control hit the tent instead of terrain.
    $roof=New-TerrainPuddleStockRoof -SendCommand ${function:Send} -X $dry.x -Z $dry.z -RoofLift 6;$result.roof=$roof
    $coveredBefore=Patch $dry.x $dry.z;$negative=Shoot 'covered-ground'
    Require (!$negative.changed -and $negative.sheltered -and $negative.budget -and $negative.before -eq $negative.after) 'Real covered terrain shot did not prove actual roof exclusion.'
    Require ([Math]::Sqrt([Math]::Pow($negative.x-$dry.x,2)+[Math]::Pow($negative.z-$dry.z,2)) -le .25) 'Covered shot left the audited central roof patch.'
    Compare-Patches $coveredBefore (Patch $dry.x $dry.z) 'Covered actual shot changed stored snow cells.';Capture-Views 'roof-negative' $views
    Remove-TerrainPuddleStockRoof ${function:Send}
    if ($Refill) {
        $centre=$result.stages['snow-positive'].shape.centre;$start=Clock;$null=Send @{cmd='dev_snow';action='storm'};Exec 'setAccTime 4'
        $until=[DateTime]::UtcNow.AddSeconds(30)
        do {Start-Sleep -Milliseconds 200;Require ([DateTime]::UtcNow -lt $until) 'Actual snowfall refill did not advance.'} while ((Clock)-$start -lt 20000)
        Exec 'setAccTime 0';$new=Send @{cmd='dev_snow';action='sample';x=$centre.x;z=$centre.z};$state=Send @{cmd='dev_snow';action='state'}
        Require ($state.falling -and $state.depth -gt $Depth -and $new.deficit -lt $centre.deficit-.01) 'Actual falling precipitation failed to refill the hole.'
        $result.refill=@{actualState=$state;centreBefore=$centre;centreAfter=$new;elapsedMs=(Clock)-$start};Capture-Views 'falling-refill' $views
    }
    CloseOwned;$result.status='actual-shot-snow-storage-gates-passed-visual-hard-object-sea-review-open'
} catch {$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=@{line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace};throw}
finally {
    if ($p -and !$p.HasExited) {try {CloseOwned} catch {$result.status='failed';$result.cleanupError=$_.Exception.Message;if (!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if ($client) {$client.Dispose()};foreach($key in $keys) {Restore-Environment $key $saved[$key]}
    try {$result.after=Pair;Require ($before.stamp -ceq $result.after.stamp -and $before.exe -ceq $result.after.exe -and $before.dll -ceq $result.after.dll) 'Installed pair/provenance changed during campaign.'} catch {$result.status='failed';$result.provenanceError=$_.Exception.Message}
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Actual snow bullet evidence: $output"
    if ($result.status -eq 'failed') {throw ($result.error+' '+$result.cleanupError+' '+$result.provenanceError)}
}
