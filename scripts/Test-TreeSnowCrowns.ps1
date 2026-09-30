# Installed individual-crown smoke fixture. Run through with-game-lock.sh.
[CmdletBinding()]
param(
    [ValidateSet('Auto','LegacyCwa','Native')][string]$World = 'Auto',
    [string]$Model = '',
    [string]$SeedCamera = '',
    [ValidateRange(0.04,0.5)][double]$Depth = 0.18,
    [ValidateRange(1,10)][int]$SettleSeconds = 2,
    [ValidateRange(1,64)][int]$DifferenceThreshold = 8,
    [switch]$Direct,
    [switch]$SelfTest,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'stock-crowns',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Finite([double]$value) { return ![double]::IsNaN($value) -and ![double]::IsInfinity($value) }
function Number([string]$text) {
    $value = [double]::Parse($text,[Globalization.NumberStyles]::Float,$culture)
    Require (Finite $value) 'Nonfinite fixture or telemetry number.'; return $value
}
function Model-Key([string]$value) { return $value.Replace('\','/').ToLowerInvariant() }
function Camera-Numbers([string]$value) {
    $parts = @($value.Trim() -split '\s+')
    Require ($parts.Count -eq 5) 'Camera requires X Z absolute height azimuth elevation.'
    return @($parts | ForEach-Object { Number $_ })
}
function Camera-Pose([double]$X,[double]$Z,[double]$Height,[double]$Azimuth,[double]$Elevation) {
    $values = @($X,$Z,$Height,$Azimuth,$Elevation)
    foreach ($value in $values) { Require (Finite $value) 'Nonfinite camera.' }
    return ($values | ForEach-Object { $_.ToString('R',$culture) }) -join ' '
}
function Crown-Camera($target,[double]$distance,[double]$azimuth,[double]$ground) {
    Require ($distance -gt 0 -and $target.top.Count -eq 3) 'Invalid crown camera target.'
    $angle = $azimuth * [Math]::PI / 180
    $height = [double]$target.maxY-[double]$target.minY
    $aimY = [double]$target.top[1]-0.15-0.2*$height
    $eyeY = [Math]::Max($aimY+[Math]::Max(3,0.18*$distance),$ground+5)
    $x = [double]$target.top[0]-[Math]::Sin($angle)*$distance
    $z = [double]$target.top[2]-[Math]::Cos($angle)*$distance
    $elevation = [Math]::Atan2($aimY-$eyeY,$distance)*180/[Math]::PI
    return Camera-Pose $x $z $eyeY $azimuth $elevation
}
function Decode-Eval([string]$display) {
    $display = $display.Trim()
    if ($display.StartsWith('"')) {
        Require ($display.Length -ge 2 -and $display.EndsWith('"')) 'Malformed SQF string.'
        $inner = $display.Substring(1,$display.Length-2); $decoded = [Text.StringBuilder]::new()
        for ($i=0; $i -lt $inner.Length; ++$i) {
            if ($inner[$i] -eq '"') {
                Require ($i+1 -lt $inner.Length -and $inner[$i+1] -eq '"') 'Malformed doubled SQF quote.'; ++$i
            }
            $null = $decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Read-TreeTrace([string[]]$lines) {
    $bounds = @{}; $proofs = @()
    $numeric = '[-+0-9.eE]+'
    foreach ($line in $lines) {
        if ($line.Contains('Wgpu tree snow receiver:')) {
            Require ($line -match ("Wgpu tree snow receiver: model=(.+?) minY=($numeric) maxY=($numeric) scope=primary-static-individual-tree-cutout-only\s*$")) 'Unknown tree bounds telemetry grammar.'
            $path = $Matches[1]; $min = Number $Matches[2]; $max = Number $Matches[3]
            Require ($max-$min -gt 0.001) 'Tree telemetry has no admitted loaded height.'
            $key = Model-Key $path
            if ($bounds.ContainsKey($key)) {
                Require ($bounds[$key].minY -eq $min -and $bounds[$key].maxY -eq $max) 'Loaded bounds changed for the same actual model.'
            }
            $bounds[$key] = @{ model = $path; minY = $min; maxY = $max; line = $line }
        } elseif ($line.Contains('Wgpu tree snow proof:')) {
            Require ($line -match ("Wgpu tree snow proof: id=(\d+) model=(.+?) path=(direct|retained) exposed=(true|false) top=($numeric),($numeric),($numeric) rays=fire-then-view-ignore-owner budgetRemaining=(\d+)\s*$")) 'Unknown tree proof telemetry grammar.'
            $values = @($Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6],$Matches[7],$Matches[8])
            Require ([uint64]$values[0] -gt 0 -and [uint64]$values[0] -le [uint32]::MaxValue) 'Invalid actual owner birth.'
            Require ([int]$values[7] -ge 0 -and [int]$values[7] -le 7) 'Proof did not consume the bounded frame budget.'
            $proofs += @{ renderId = [uint32]$values[0]; model = $values[1]; path = $values[2]; exposed = $values[3] -ceq 'true';
                top = @((Number $values[4]),(Number $values[5]),(Number $values[6])); budgetRemaining = [int]$values[7]; line = $line }
        }
    }
    return @{ bounds = $bounds; proofs = $proofs }
}
function Read-LiveTreeLog([string]$path) {
    # The native logger keeps its write handle open. Explicit sharing permits
    # a read-only snapshot; omit a final incomplete line until the next poll.
    $stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream)
    try {
        $content=$reader.ReadToEnd();$lastNewline=$content.LastIndexOf("`n")
        if($lastNewline -lt 0){return @()}
        return $content.Substring(0,$lastNewline).Split("`n")
    } finally {$reader.Dispose()}
}
function Assert-SnowState($state,[bool]$enabled,[double]$depth,[Nullable[int]]$chunks) {
    foreach ($field in @('enabled','falling','geometry','depth','chunks')) { Require ($null -ne $state.$field) "Actual snow field missing: $field" }
    Require ($state.enabled -is [bool] -and $state.enabled -eq $enabled) 'Actual snow enabled state differs.'
    Require ($state.falling -is [bool] -and !$state.falling -and $state.geometry -is [bool] -and $state.geometry) 'Fixed snow must not fall and must retain geometry.'
    Require ((Finite ([double]$state.depth)) -and [Math]::Abs([double]$state.depth-$depth) -lt 0.00001) 'Actual deposited snow depth differs.'
    Require ($state.chunks -ge 0 -and $state.chunks -eq [Math]::Floor([double]$state.chunks)) 'Invalid actual snow chunk count.'
    if ($null -ne $chunks) { Require ($state.chunks -eq $chunks) 'Camera/switch interval changed stored chunks.' }
}
function Assert-ObjectTelemetry([string]$line,[double]$depth) {
    Require ($line -match 'wgpu object snow: enabled=1 deposit=([-+0-9.eE]+) snowlineHeight=([-+0-9.eE]+) snowlineRange=([-+0-9.eE]+) snowlineDepth=([-+0-9.eE]+)\s*$') 'Unknown effective raised-surface snow upload.'
    $values = @(1..4 | ForEach-Object { $Matches[$_] }) | ForEach-Object { Number $_ }
    Require ([Math]::Abs($values[0]-$depth) -le 0.000051 -and $values[1] -lt 0 -and $values[2] -eq 0 -and $values[3] -eq 0) 'Actual object depth/snowline differs from fixed deposited control.'
    return @{ line = $line; deposit = $values[0]; snowlineHeight = $values[1]; snowlineRange = $values[2]; snowlineDepth = $values[3] }
}
function Test-Helpers {
    $script:checks = 0
    function Check([bool]$ok,[string]$message) { Require $ok $message; ++$script:checks }
    function Refuses([scriptblock]$action) { $refused=$false; try { & $action | Out-Null } catch { $refused=$true }; Check $refused 'Invalid helper input accepted.' }
    $path = 'data3d\str buk.p3d'
    $bound = "Wgpu tree snow receiver: model=$path minY=-6 maxY=14 scope=primary-static-individual-tree-cutout-only"
    $proof = "Wgpu tree snow proof: id=123 model=$path path=retained exposed=true top=4320.123,114.150,4270.125 rays=fire-then-view-ignore-owner budgetRemaining=7"
    $trace = Read-TreeTrace @(('[INFO] '+$bound),('[INFO] '+$proof))
    Check ($trace.bounds[(Model-Key $path)].maxY -eq 14 -and $trace.proofs.Count -eq 1 -and $trace.proofs[0].exposed) 'Actual grammar/path with spaces changed.'
    Check ($trace.proofs[0].top[1] -eq 114.15 -and $trace.proofs[0].renderId -eq 123) 'Actual proof coordinates or identity changed.'
    $negative = Read-TreeTrace @($bound,$proof.Replace('exposed=true','exposed=false').Replace('path=retained','path=direct'))
    Check (!$negative.proofs[0].exposed -and $negative.proofs[0].path -ceq 'direct') 'Covered/direct proof changed.'
    foreach ($bad in @($proof.Replace('id=123','id=0'),$proof.Replace('budgetRemaining=7','budgetRemaining=8'),
        $proof.Replace('exposed=true','exposed=1'),$proof.Replace('path=retained','path=unknown'),$bound.Replace('maxY=14','maxY=-6'))) { Refuses { Read-TreeTrace @($bad) } }
    Refuses { Read-TreeTrace @($bound,$bound.Replace('maxY=14','maxY=15')) }
    Refuses { Read-TreeTrace @($proof.Replace('id=123','id=4294967296')) }
    Refuses { Read-TreeTrace @($proof.Replace('top=4320.123','top=NaN')) }
    Refuses { Read-TreeTrace @($bound.Replace('maxY=14','maxY=1e9999')) }
    Check ((Decode-Eval '"data3d\str buk.p3d"') -ceq $path) 'SQF raw backslashes were unescaped.'
    Check ((Decode-Eval '"a""b"') -ceq 'a"b') 'SQF doubled quote changed.'
    Refuses { Decode-Eval 'UNKNOWN COMMAND' }; Refuses { Decode-Eval '"bad"quote"' }
    $old = [Threading.Thread]::CurrentThread.CurrentCulture
    try {
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('de-DE')
        Check ((Camera-Pose 1.25 2.5 3.75 4.5 -5.25) -ceq '1.25 2.5 3.75 4.5 -5.25') 'Camera depends on local culture.'
        $target=@{top=@(100,30,200);minY=-6;maxY=14}
        $cam=Camera-Numbers (Crown-Camera $target 150 0 0)
        Check ($cam[0] -eq 100 -and $cam[1] -eq 50 -and $cam[2] -gt 30 -and $cam[4] -lt 0) 'Derived far camera changed its actual target.'
    } finally { [Threading.Thread]::CurrentThread.CurrentCulture=$old }
    Refuses { Camera-Numbers '1 2 3' }; Refuses { Number 'NaN' }
    $state=[pscustomobject]@{enabled=$true;falling=$false;geometry=$true;depth=0.18;chunks=2}
    Assert-SnowState $state $true 0.18 2; ++$script:checks
    Refuses { Assert-SnowState $state $false 0.18 2 }; Refuses { Assert-SnowState $state $true 0.2 2 }
    $upload='wgpu object snow: enabled=1 deposit=0.1800 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000'
    Check ((Assert-ObjectTelemetry $upload 0.18).deposit -eq 0.18) 'Actual uploaded deposit changed.'
    Refuses { Assert-ObjectTelemetry $upload.Replace('enabled=1','enabled=0') 0.18 }
    Refuses { Assert-ObjectTelemetry $upload.Replace('snowlineDepth=0.0000','snowlineDepth=0.0600') 0.18 }
    Refuses { Assert-ObjectTelemetry ($upload+' unexpected=1') 0.18 }
    Write-Host "Tree crown actual helper checks passed: $checks. No game/GPU/filesystem fixture used."
}
if ($SelfTest) { Test-Helpers; return }
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
function Installed-State {
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    Require ([bool]$stamp) 'Installed provenance is empty.'
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name=$_; path=$file.FullName; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash; bytes=$file.Length; writtenUtc=$file.LastWriteTimeUtc.ToString('o') }
    }
    return @{deployedFrom=$stamp;files=@($files)}
}
$before=Installed-State
$mission=Join-Path $root 'tests/perf/missions/perf_field.eden'
Require (Test-Path -LiteralPath $mission) 'Stock Eden mission missing.'
if ($World -eq 'Native') { Require (Test-Path -LiteralPath $NativeAddons) 'Native fallback archives missing.' }
$output=Join-Path $root ('build/tree-snow/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$result=[ordered]@{status='running';before=$before;attempts=@{};arms=@{};comparisons=@{};error=$null;
    scope='Actual model/probe/state and owned capture evidence. Whole central-ROI differences do not segment crown snow or accept trunks/holes/far quality.'}
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE',
    'WGR_OBJECT_SNOW','WGR_TREE_SNOW','WGR_TREE_SNOW_TRACE','WGR_SNOW_POWDER','WGR_SNOW_SURFACE_FIXTURE','WGR_GPU_DRIVEN',
    'WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GRASS','POSEIDON_WIND_OVERRIDE','WGR_CLOUD_COVERAGE',
    'WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER','WGR_INTERIOR_SKY','WGR_INTERIOR_SKY_DEBUG','POSEIDON_INTERIOR_SKY_PROBE',
    'WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLES','WGR_GROUND_PUDDLES','WGR_WEATHER_COVER','WGR_WEATHER_COVER_TRACE')
$saved=@{}; foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
@{sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
    helperSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
    metricSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs')).Hash;
    installed=$before;requestedWorld=$World;requestedModel=$Model;requestedSeed=$SeedCamera;direct=[bool]$Direct;lockOwner=$env:LOCK_OWNER;
    arms=@('deposit-tree-on','deposit-tree-off');depth=$Depth;snowline='off';date=@(1985,6,21,16,0);time=100;exposure=1;
    rain=0;cloud=0;wind='0 90 0';temporal=0;jitter=0;grass=0;differenceThreshold=$DifferenceThreshold;
    lookup='Actual positive owner proof + loaded bounds, then actual nearest SQF object with matching debug model and tight XZ/paired ASL identity.'} |
    ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
if (!('TerrainPuddleMetrics' -as [type])) {
    $refs=@([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    $refs+=@([Drawing.Bitmap].Assembly.GetReferencedAssemblies() | Where-Object Name -like 'System.Private.Windows.*' | ForEach-Object{[Reflection.Assembly]::Load($_).Location})
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $refs
}
$p=$null;$client=$null;$reader=$null;$writer=$null;$log=$null;$armDir=$null
function Assert-RunHealth {
    if($p -and $p.HasExited){throw "Owned game exited early: $($p.ExitCode)"}
    if($log -and (Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot|Cannot load --test-world' -Quiet)){throw 'Installed runtime/mission/world failed.'}
}
function Send($command) {
    Assert-RunHealth;$request=$command|ConvertTo-Json -Compress
    $request|Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$writer.WriteLine($request)
    do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed.'};$line|Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
    if(!$reply.ok){throw $line};return $reply
}
function Eval([string]$code){$reply=Send @{cmd='eval';code=$code};Require ($null -ne $reply.result) "No eval result: $code";return Decode-Eval ([string]$reply.result)}
function Snow-State([bool]$enabled,[double]$depth,[Nullable[int]]$chunks){$s=Send @{cmd='dev_snow';action='state'};Assert-SnowState $s $enabled $depth $chunks;return $s}
function Close-Owned {
    if ($p) {
        if (!$p.HasExited) {
            $null = Send @{cmd='exec';code='setAccTime 1'}
            $null = Send @{cmd='exit'}
            Require ($p.WaitForExit(20000)) 'Owned normal exit timed out.'
        }
        Require ($p.ExitCode -eq 0) 'Owned normal exit failed.'
        Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Shutdown marker absent.'
        Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Owned run contains a runtime failure.'
        $result.attempts[$worldArm].arms[$arm].normalExit = @{exitCode=$p.ExitCode;shutdownComplete=$true}
    }
    if ($client) { $client.Dispose();$script:client=$null }
    $script:reader=$null;$script:writer=$null
}
function Bind-Target($target,[bool]$paired) {
    # nearestObject/GetPos expects terrain-relative height, whereas telemetry
    # top/origin are ASL. Search at the proven XZ on the ground; the actual
    # model and exact ASL identity below still determine acceptance.
    $point=(@($target.top[0],$target.top[2],0)|ForEach-Object{([double]$_).ToString('R',$culture)})-join ','
    $null=Send @{cmd='exec';code=('treeSnowTarget=nearestObject ['+$point+']')}
    if((Eval 'isNull treeSnowTarget') -ne $false){throw 'FixtureUnavailable: actual nearest tree object not found.'}
    $debug=[string](Send @{cmd='eval';code='treeSnowTarget'}).result
    $basename=[IO.Path]::GetFileNameWithoutExtension($target.model.Replace('\','/'))
    $pattern='^(?:NOID|\d+:)\s+'+[regex]::Escape($basename)+'(?:\.p3d|\.xob)?$'
    if($debug.Trim('"') -notmatch $pattern){throw "FixtureUnavailable: actual nearest object '$debug' does not identify admitted model '$($target.model)'."}
    $position=Eval 'getPosASL treeSnowTarget'
    Require ($position -is [array] -and $position.Count -eq 3) 'Tree returned no actual ASL.'
    foreach($v in $position){Require (Finite ([double]$v)) 'Tree ASL is nonfinite.'}
    # Recentered upright CWA model upper-centre is at its actual origin XZ.
    if([Math]::Abs([double]$position[0]-[double]$target.top[0]) -gt 0.25 -or [Math]::Abs([double]$position[1]-[double]$target.top[2]) -gt 0.25){throw 'FixtureUnavailable: actual nearest object does not bind the proven crown XZ.'}
    if($paired){for($i=0;$i -lt 3;++$i){Require ([Math]::Abs([double]$position[$i]-[double]$target.actualPosition[$i]) -lt 0.02) 'ON/OFF target ASL identity differs.'}}
    return @{debugName=$debug;position=$position}
}
function Screenshot([string]$name,$target) {
    $current=Eval 'getPosASL treeSnowTarget'
    Require ($current -is [array] -and $current.Count -eq 3) 'Capture target has no actual ASL.'
    foreach ($value in $current) { Require (Finite ([double]$value)) 'Capture target ASL is nonfinite.' }
    for($i=0;$i -lt 3;++$i){Require ([Math]::Abs([double]$current[$i]-[double]$target.actualPosition[$i]) -lt 0.001) 'Static capture target moved.'}
    Require ([Math]::Abs([double](Eval 'time')-100) -lt 0.001) 'Fixed sim time changed.'
    $path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path -LiteralPath $path)){Assert-RunHealth;Require ([DateTime]::UtcNow -lt $until) 'Screenshot not written.';Start-Sleep -Milliseconds 100}
    $bytes=[IO.File]::ReadAllBytes($path);Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG.'
    return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;simTime=100;targetPosition=$current}
}
function Capture-Views([string]$stage,$views,$target) {
    $captures=@{};foreach($view in $views.GetEnumerator()){
        Require ((Eval ('triFreeFlyPose "'+$view.Value+'"')) -ceq 'OK') "Camera refused: $($view.Key)"
        Start-Sleep -Seconds $SettleSeconds;$shot=Screenshot ($stage+'-'+$view.Key) $target;$shot.pose=$view.Value;$captures[$view.Key]=$shot
    };return $captures
}
function Pair($first,$second){return [TerrainPuddleMetrics]::Compare($first.path,$second.path,$DifferenceThreshold)}
try {
    foreach($key in @('POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}
    $env:POSEIDON_SNOW_TEST_DEPTH='0';$env:POSEIDON_SNOWLINE='off';$env:WGR_OBJECT_SNOW='1';$env:WGR_TREE_SNOW_TRACE='1';$env:WGR_SNOW_SURFACE_FIXTURE='1'
    $env:WGR_GPU_DRIVEN=if($Direct){'0'}else{'1'};$env:WGR_SNOW_POWDER='1';$env:WGR_TEMPORAL='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='1'
    $env:WGR_GRASS='0';$env:POSEIDON_WIND_OVERRIDE='0 90 0';$env:WGR_CLOUD_COVERAGE='0';$env:WGR_LOD_GOVERNOR_RANGE='1';$env:WGR_TERRAIN_JITTER='0'
    $env:WGR_INTERIOR_SKY='1';$env:WGR_INTERIOR_SKY_DEBUG='0';$env:WGR_TERRAIN_PUDDLES='0';$env:WGR_GROUND_PUDDLES='0'
    $env:WGR_WEATHER_COVER='1';$env:WGR_WEATHER_COVER_TRACE='1'
    $worlds=if($World -eq 'Auto'){@('LegacyCwa','Native')}else{@($World)};$selected=$null;$views=$null
    foreach($worldArm in $worlds){
        $selected=$null;$views=$null;$result.arms=@{};$result.comparisons=@{}
        if($worldArm -eq 'Native'){Require (Test-Path -LiteralPath $NativeAddons) 'Stock fixture unavailable and native fallback archives missing.';$env:POSEIDON_REFORGER_WORLD='worlds/eden';$env:POSEIDON_REFORGER_OBJECTS='1';$env:POSEIDON_REFORGER_STREAM='1'}
        else{foreach($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}}
        $seed=if($SeedCamera){$SeedCamera}elseif($worldArm -eq 'LegacyCwa'){'4320.38 4270.16 109.82 306.4 -0.1'}else{'4697.60 3998.02 50.23 220.6 -5'}
        $seedValues=Camera-Numbers $seed;$result.attempts[$worldArm]=@{status='running';seed=$seed;refusals=@();arms=@{}}
        try{
            foreach($arm in @('deposit-tree-on','deposit-tree-off')){
                Require (!(Get-Process OpenPoseidon -ErrorAction SilentlyContinue)) 'Unexpected game process between arms.'
                $armDir=Join-Path $output ($worldArm+'-'+$arm);$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force -Path $profile|Out-Null
                $log=Join-Path $armDir 'engine.log';$env:POSEIDON_USER_DIR=$profile;$env:WGR_TREE_SNOW=if($arm -eq 'deposit-tree-on'){'1'}else{'0'}
                [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
                $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
                $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly')+($seed -split '\s+')+@('--log-file',('"'+$log+'"'))
                if($worldArm -eq 'Native'){$arguments+=@('--test-world',('"'+$NativeAddons+'"'))}
                $stdout=Join-Path $armDir 'stdout.txt';$stderr=Join-Path $armDir 'stderr.txt'
                $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr;$null=$p.Handle
                $result.attempts[$worldArm].arms[$arm]=@{status='starting';pid=$p.Id;log=$log;arguments=$arguments;treeSwitch=$env:WGR_TREE_SNOW}
                $until=[DateTime]::UtcNow.AddSeconds(120)
                do{Assert-RunHealth;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable after120s.';Start-Sleep -Milliseconds 250}}while(!$client)
                $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
                Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Scene not ready.'
                $null=Send @{cmd='exec';code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]'}
                Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Fixed sim time refused.';Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Fixed brightness refused.'
                $null=Send @{cmd='dev_snow';action='disable'};$initial=Snow-State $false 0 0
                $null=Send @{cmd='dev_snow';action='enable'};$null=Send @{cmd='dev_snow';action='deposit';metres=$Depth};$enabled=Snow-State $true $Depth $null
                if($arm -eq 'deposit-tree-on'){
                    $deadline=[DateTime]::UtcNow.AddSeconds(20);$selected=$null
                    do{
                        Assert-RunHealth;$trace=Read-TreeTrace @(Read-LiveTreeLog $log);$candidates=@()
                        foreach($proof in $trace.proofs){
                            $key=Model-Key $proof.model;$extension=if($worldArm -eq 'LegacyCwa'){'.p3d'}else{'.xob'}
                            if(!$proof.exposed -or !$key.EndsWith($extension) -or !$trace.bounds.ContainsKey($key) -or ($Model -and $key -cne (Model-Key $Model))){continue}
                            if($Direct -and $proof.path -cne 'direct'){continue}
                            if(!$Direct -and $proof.path -cne 'retained'){continue}
                            $distance=([double]$proof.top[0]-$seedValues[0])*([double]$proof.top[0]-$seedValues[0])+([double]$proof.top[2]-$seedValues[1])*([double]$proof.top[2]-$seedValues[1])
                            if($distance -gt 1000000){continue}
                            $candidate=@{model=$proof.model;renderId=$proof.renderId;path=$proof.path;top=$proof.top;minY=$trace.bounds[$key].minY;maxY=$trace.bounds[$key].maxY;proof=$proof.line;bounds=$trace.bounds[$key].line;distanceSquared=$distance}
                            $candidates+=$candidate
                        }
                        foreach($candidate in @($candidates|Sort-Object distanceSquared)){
                            try{$bound=Bind-Target $candidate $false;$candidate.actualPosition=$bound.position;$candidate.debugName=$bound.debugName;$selected=$candidate;break}
                            catch{if(!$_.Exception.Message.StartsWith('FixtureUnavailable:')){throw};$result.attempts[$worldArm].refusals+=$_.Exception.Message}
                        }
                        if(!$selected){Start-Sleep -Milliseconds 200}
                    }while(!$selected -and [DateTime]::UtcNow -lt $deadline)
                    if(!$selected){throw 'FixtureUnavailable: no exposed actual admitted individual tree with loaded bounds and matching SQF identity in the bounded candidate region.'}
                    $views=[ordered]@{}
                    foreach($distance in @(30,150,400)){
                        $angle=$seedValues[3]*[Math]::PI/180;$cx=[double]$selected.top[0]-[Math]::Sin($angle)*$distance;$cz=[double]$selected.top[2]-[Math]::Cos($angle)*$distance
                        $ground=Get-TerrainPuddleFixtureHeight {param($request) Send $request} $cx $cz
                        $views[('distance'+$distance)]=Crown-Camera $selected $distance $seedValues[3] $ground
                    }
                    $height=[double]$selected.maxY-[double]$selected.minY;$aim=[double]$selected.top[1]-0.15-0.2*$height
                    $views.topShot=Camera-Pose $selected.top[0] ([double]$selected.top[2]-5) ([double]$selected.top[1]+40) 0 ([Math]::Atan2($aim-([double]$selected.top[1]+40),5)*180/[Math]::PI)
                    $views.returnNear=$views.distance30
                }else{
                    $bound=Bind-Target $selected $true;Start-Sleep -Seconds 1;$trace=Read-TreeTrace @(Read-LiveTreeLog $log);$key=Model-Key $selected.model
                    Require ($trace.bounds.ContainsKey($key) -and $trace.bounds[$key].minY -eq $selected.minY -and $trace.bounds[$key].maxY -eq $selected.maxY) 'OFF loaded model bounds differ or are unavailable.'
                    Require ($trace.proofs.Count -eq 0) 'Tree-OFF arm unexpectedly issued crown roof probes.'
                }
                $captures=@{};$captures.enabled=Capture-Views 'enabled' $views $selected
                $telemetry=@(Select-String -LiteralPath $log -Pattern 'wgpu object snow: enabled='|ForEach-Object{$_.Line});Require ($telemetry.Count -gt 0) 'Actual raised-surface upload absent.'
                $uploaded=Assert-ObjectTelemetry $telemetry[-1] $Depth;$afterViews=Snow-State $true $Depth ([int]$enabled.chunks)
                $null=Send @{cmd='dev_snow';action='disable'};$disabled=Snow-State $false $Depth ([int]$enabled.chunks);$captures.disabled=Capture-Views 'disabled' $views $selected
                $null=Send @{cmd='dev_snow';action='enable'};$restored=Snow-State $true $Depth ([int]$enabled.chunks)
                $captures.restored=Capture-Views 'restored' ([ordered]@{distance30=$views.distance30;distance400=$views.distance400}) $selected
                $weather=Send @{cmd='weather_visibility'};Require ($null -ne $weather.rain -and $null -ne $weather.fog -and $weather.rain -le 0.001 -and $weather.fog -le 0.001) 'Actual fixed weather differs.'
                $null=Send @{cmd='dev_snow';action='disable'};$final=Snow-State $false $Depth ([int]$enabled.chunks)
                $armResult=@{status='captured';world=$worldArm;pid=$p.Id;log=$log;arguments=$arguments;treeSwitch=$env:WGR_TREE_SNOW;target=$selected;actualBinding=$bound;
                    views=$views;captures=$captures;initial=$initial;enabled=$enabled;afterViews=$afterViews;disabled=$disabled;restored=$restored;finalDisabled=$final;weather=$weather;objectUpload=$uploaded;
                    treeTrace=(Read-TreeTrace @(Read-LiveTreeLog $log));stdout=$stdout;stderr=$stderr;proofScope='ON actual admitted source and real positive upper-crown query; OFF binds actual same position/debug model/bounds without issuing queries. Captures do not prove semantic snow masks.'}
                $result.arms[$arm]=$armResult;$result.attempts[$worldArm].arms[$arm]=$armResult
                $result.comparisons[$arm+'-return']=Pair $captures.enabled.distance30 $captures.enabled.returnNear
                $result.comparisons[$arm+'-restoreNear']=Pair $captures.enabled.distance30 $captures.restored.distance30
                $result.comparisons[$arm+'-restoreFar']=Pair $captures.enabled.distance400 $captures.restored.distance400
                Close-Owned
            }
            foreach($view in $views.Keys){$result.comparisons[$view]=Pair $result.arms['deposit-tree-on'].captures.enabled[$view] $result.arms['deposit-tree-off'].captures.enabled[$view]}
            foreach($view in @('distance30','distance150','distance400')){Require ($result.comparisons[$view].ChangedPixels -gt 0) "No ON/OFF pixel positive control in crown-centred ROI at $view; source proof alone cannot accept an unused coat."}
            $result.selectedWorld=$worldArm;$result.attempts[$worldArm].status='captured';break
        }catch{
            $result.attempts[$worldArm].error=$_.Exception.Message
            if($World -eq 'Auto' -and $worldArm -eq 'LegacyCwa' -and !$selected -and $_.Exception.Message.StartsWith('FixtureUnavailable:')){
                $result.attempts[$worldArm].status='fixture-unavailable';Close-Owned;continue
            }
            $result.attempts[$worldArm].status='failed';throw
        }
    }
    Require ($null -ne $result.selectedWorld) 'No actual stock/native crown fixture completed.'
    $result.status='captured-not-visually-accepted'
}catch{$result.status='failed';$result.error=$_.Exception.Message;throw}
finally{
    if($p -and !$p.HasExited){try{Close-Owned}catch{};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(5000);$result.forcedOwnedProcessStop=$true}}
    if($client){$client.Dispose()}
    try{
        $after=Installed-State;$result.after=$after;$changed=$after.deployedFrom -cne $before.deployedFrom
        for($i=0;$i -lt $before.files.Count;++$i){foreach($field in @('name','path','sha256','bytes','writtenUtc')){if($after.files[$i][$field] -cne $before.files[$i][$field]){$changed=$true}}}
        if($changed){$result.status='failed';$result.error='Installed pair/provenance changed during campaign.'}
    }catch{$result.status='failed';$result.error=$_.Exception.Message}
    foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result|ConvertTo-Json -Depth 22|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Tree crown evidence: $output"
    if($result.status -eq 'failed'){throw $result.error}
    Write-Host 'Actual source, state, pair and capture controls complete; inspect crowns/trunks/alpha/LOD/far glitter before accepting appearance.'
}
