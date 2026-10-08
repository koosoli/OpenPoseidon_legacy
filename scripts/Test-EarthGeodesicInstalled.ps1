<# Installed free-flight acceptance. Owner runs through with-game-lock.sh after
   a matched deployment. SDL D crosses near/orbital rebase thresholds; only the
   PID launched here is stopped. -SelfTest is CPU-only. -Offline requires the
   initial and rebased elevation windows already in the bounded external cache. #>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
    [ValidateRange(60,240)][int]$PatchTimeoutSeconds=150,
    [switch]$Offline,
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
$radius=6378137.0;$anchor=6400.0
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($value){$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite runtime value';return $n}
function Fmt($value){return (Num $value).ToString('R',$culture)}
function Decode([string]$value){
    $value=$value.Trim()
    if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete SQF string';return $value.Substring(1,$value.Length-2).Replace('""','"')}
    return $value
}
function ParseLocation([string]$value){
    Require ($value-match'^OK:latitude=([-+0-9.eE]+) longitude=([-+0-9.eE]+) altitude=([-+0-9.eE]+) revision=([0-9]+)$') ('Malformed origin: '+$value)
    return @{latitude=(Num $Matches[1]);longitude=(Num $Matches[2]);altitude=(Num $Matches[3]);revision=[uint64]$Matches[4];raw=$value}
}
function Ecef($location,$vector){
    Require ($vector.Count-eq3) 'Missing three-dimensional camera vector'
    $lat=$location.latitude*[Math]::PI/180;$lon=$location.longitude*[Math]::PI/180
    $x=Num $vector[0];$y=Num $vector[1];$z=Num $vector[2]
    return @((-[Math]::Sin($lon)*$x+[Math]::Cos($lat)*[Math]::Cos($lon)*$y-[Math]::Sin($lat)*[Math]::Cos($lon)*$z),
        ([Math]::Sin($lat)*$y+[Math]::Cos($lat)*$z),
        ([Math]::Cos($lon)*$x+[Math]::Cos($lat)*[Math]::Sin($lon)*$y-[Math]::Sin($lat)*[Math]::Sin($lon)*$z))
}
function Difference($a,$b){return @(((Num $a[0])-(Num $b[0])),((Num $a[1])-(Num $b[1])),((Num $a[2])-(Num $b[2])))}
function Dot($a,$b){return (Num $a[0])*(Num $b[0])+(Num $a[1])*(Num $b[1])+(Num $a[2])*(Num $b[2])}
function Length($v){return [Math]::Sqrt((Dot $v $v))}
function PhysicalPose($location,$camera){
    Require ($camera.position.Count-eq3 -and $camera.direction.Count-eq3 -and $camera.up.Count-eq3) 'Actual camera pose missing'
    return @{position=(Ecef $location @(((Num $camera.position[0])-$anchor),($radius+(Num $camera.position[1])),((Num $camera.position[2])-$anchor)));direction=(Ecef $location $camera.direction);up=(Ecef $location $camera.up)}
}
function CheckContinuity($before,$after,[double]$maxDistance){
    $a=PhysicalPose $before.location $before.camera;$b=PhysicalPose $after.location $after.camera
    $delta=Difference $b.position $a.position;$east=Ecef $before.location @(1,0,0)
    $along=Dot $delta $east;$perpendicular=[Math]::Sqrt([Math]::Max(0,(Dot $delta $delta)-$along*$along))
    # D moves along the original physical east axis. Origin changes must not
    # teleport the camera off that line, rotate the view, or roll its up vector.
    Require ($along-gt1 -and $along-lt$maxDistance) ('Actual SDL movement missing/unbounded: '+$along)
    Require ($perpendicular-lt10) ('Rebase changed physical position off the movement line: '+$perpendicular)
    $directionError=Length (Difference $b.direction $a.direction);$upError=Length (Difference $b.up $a.up)
    Require ($directionError-lt.0001 -and $upError-lt.0001) ('Rebase rotated actual view/up: '+$directionError+', '+$upError)
    return @{beforeECEF=$a;afterECEF=$b;eastMovementMetres=$along;perpendicularErrorMetres=$perpendicular;directionError=$directionError;upError=$upError}
}
if($SelfTest){
    $origin=ParseLocation 'OK:latitude=0.00000000 longitude=179.50000000 altitude=1000.000 revision=3'
    Require ($origin.revision-eq3 -and $origin.longitude-eq179.5) 'Origin parser failed'
    Require ((Decode '"{""covered"":false}"')-ceq'{"covered":false}') 'SQF decoding failed'
    Require ([Math]::Abs((Length (Ecef $origin @(3,4,0)))-5)-lt1e-12) 'ECEF basis is not orthonormal'
    $a=@{location=@{latitude=0;longitude=0};camera=@{position=@(6400,1000,6400);direction=@(0,0,1);up=@(0,1,0)}}
    $b=@{location=@{latitude=0;longitude=0};camera=@{position=@(6500,1000,6400);direction=@(0,0,1);up=@(0,1,0)}}
    Require ((CheckContinuity $a $b 1000).eastMovementMetres-eq100) 'Continuity check rejected straight input'
    foreach($bad in @(@(6500,1100,6400),@(6500,1000,6500))){
        $b.camera.position=$bad;$rejected=$false;try{$null=CheckContinuity $a $b 1000}catch{$rejected=$true}
        Require $rejected 'Continuity check accepted a physical jump'
    }
    $dx=32090.0;$up=$radius+1000;$lon=[Math]::Atan2($dx,$up)
    $a.camera.position=@(($anchor+31990),1000,$anchor)
    $b=@{location=@{latitude=0;longitude=($lon*180/[Math]::PI)};camera=@{position=@($anchor,([Math]::Sqrt($dx*$dx+$up*$up)-$radius),$anchor);direction=@(0,0,1);up=@((-[Math]::Sin($lon)),([Math]::Cos($lon)),0)}}
    Require ([Math]::Abs((CheckContinuity $a $b 1000).eastMovementMetres-100)-lt1e-7) 'Continuity check rejected a correct spherical rebase'
    $b.camera.direction=@(1,0,0);$rejected=$false;try{$null=CheckContinuity $a $b 1000}catch{$rejected=$true}
    Require $rejected 'Continuity check accepted a rotated physical view'
    Write-Host 'Earth geodesic installed-test parser/ECEF/continuity negative controls PASS';return
}
Require ($ExpectedCommit -and $env:LOCK_OWNER) 'ExpectedCommit and scripts/with-game-lock.sh are required'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved'
$stampPath=Join-Path $GameDir 'DEPLOYED-FROM.txt';$stamp=(Get-Content -LiteralPath $stampPath -Raw).Trim()
Require ($stamp.Contains($ExpectedCommit)) 'Installed source differs from ExpectedCommit'
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg'
Require (Test-Path -LiteralPath $imagery) 'Existing cached NASA overview required; this test does not download imagery'
$repo=Split-Path -Parent $PSScriptRoot
$out=Join-Path $repo ('build/earth-geodesic-installed/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
$null=New-Item -ItemType Directory -Force -Path $out
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $CacheDirectory
$mission=Join-Path $CacheDirectory 'fixture/earth-preview.abel'
$log=Join-Path $out 'engine.log';$stderr=Join-Path $out 'stderr.log'
$settings=@{POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='46.6';POSEIDON_EARTH_LONGITUDE='8.1';POSEIDON_EARTH_CACHE=$CacheDirectory;POSEIDON_EARTH_IMAGERY=$imagery;
    POSEIDON_EARTH_FEATURES='0';POSEIDON_EARTH_BUILDINGS='0';POSEIDON_EARTH_SURFACE='0';POSEIDON_EARTH_CONTENT_URL='';
    WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';POSEIDON_USER_DIR=(Join-Path $out 'user')}
if($Offline){$settings.HTTPS_PROXY='http://127.0.0.1:9';$settings.HTTP_PROXY='http://127.0.0.1:9';$settings.ALL_PROXY='http://127.0.0.1:9';$settings.NO_PROXY=''}
$saved=@{};$p=$null;$client=$null;$writer=$null;$reader=$null;$heldKey=$null
$result=@{installed=$stamp;cacheDirectory=$CacheDirectory;offline=[bool]$Offline;passed=$false;arms=@();scope='Actual Earth free-flight only; finite mission entities/navigation are not globally rebased. Screenshots require inspection.'}
$overall=[DateTime]::UtcNow.AddSeconds(200+2*$PatchTimeoutSeconds)
function Health{Require ($p -and !$p.HasExited) 'Owned game exited';Require ([DateTime]::UtcNow-lt$overall) 'Overall acceptance deadline'}
function Send($command){
    Health;$writer.WriteLine(($command|ConvertTo-Json -Depth 10 -Compress))
    do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
    Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Location{return ParseLocation (Eval 'triEarthLocation []')}
function Ground([double]$x,[double]$z){
    $g=(Eval ('triEarthGround ['+(Fmt $x)+','+(Fmt $z)+']'))|ConvertFrom-Json
    Require ($g.covered -is [bool] -and $g.revision-ge1) 'Malformed resident-ground diagnostic';return $g
}
function WaitFrames([int]$count){
    $start=Num (Eval 'triFrameCount');$until=[DateTime]::UtcNow.AddSeconds(12)
    do{Require ([DateTime]::UtcNow-lt$until) 'Rendered frame deadline';Start-Sleep -Milliseconds 30}while((Num (Eval 'triFrameCount'))-lt$start+$count)
}
function Snapshot{
    $first=Location;$camera=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera;$last=Location
    Require ($first.revision-eq$last.revision) 'Snapshot crossed an unexpected origin change with keys released'
    return @{location=$last;camera=$camera;frame=(Num (Eval 'triFrameCount'))}
}
function PatchRows{
    foreach($row in @(Select-String -LiteralPath $log -Pattern 'Earth terrain patch ')){
        if($row.Line-match'Earth terrain patch ([0-9]+): .*revision ([0-9]+)$'){
            @{generation=[uint64]$Matches[1];revision=[uint64]$Matches[2];lineNumber=$row.LineNumber;line=$row.Line}
        }
    }
}
function WaitGround($location,[uint64]$afterGeneration=0){
    $until=[DateTime]::UtcNow.AddSeconds($PatchTimeoutSeconds)
    do{
        $g=Ground $anchor $anchor;Require ($g.revision-eq$location.revision) 'Ground query belongs to a superseded origin'
        if($g.covered -and $g.generation-gt$afterGeneration){
            $matching=@(PatchRows|Where-Object {$_.revision-eq$location.revision -and $_.generation-eq$g.generation})
            if($matching.Count){return @{ground=$g;upload=$matching[-1]}}
        }
        Require ([DateTime]::UtcNow-lt$until) 'Current resident publication deadline; offline cache/service may lack this window; no ground acceptance established'
        Start-Sleep -Milliseconds 250
    }while($true)
}
function Capture([string]$label){
    $path=Join-Path $out ($label+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(15)
    while(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length-eq0){Health;Require ([DateTime]::UtcNow-lt$until) 'Screenshot deadline';Start-Sleep -Milliseconds 100}
    return $path
}
function RebaseArm([string]$name,[double]$altitude,[double]$threshold,[double]$maximumMovement){
    $startLocation=Location;$x=$anchor+$threshold-10
    Require ((Eval ('triFreeFlyPose "'+(Fmt $x)+' 6400 '+(Fmt $altitude)+' 0 -20"'))-ceq'OK') 'Threshold approach pose refused'
    WaitFrames 2;$before=Snapshot
    Require ($before.location.revision-eq$startLocation.revision) 'Camera rebased inside its deadband'
    Require ([Math]::Abs($before.camera.position[0]-$x)-lt.2 -and [Math]::Abs($before.camera.position[1]-$altitude)-lt2) 'Actual pre-rebase camera differs'
    $oldGround=Ground $before.camera.position[0] $before.camera.position[2]
    $arm=@{name=$name;before=$before;oldGround=$oldGround;input='Actual held SDL D, scancode7'}
    $script:result.arms+=,$arm
    $until=[DateTime]::UtcNow.AddSeconds(6);$clock=[Diagnostics.Stopwatch]::StartNew()
    try{
        $script:heldKey=7;$null=Send @{cmd='key';sc=7;hold=$true}
        do{$during=Location;Require ([DateTime]::UtcNow-lt$until) 'Actual SDL D input did not trigger a camera rebase'}while($during.revision-eq$before.location.revision)
    }finally{$null=Send @{cmd='key_up';sc=7};$script:heldKey=$null}
    $clock.Stop();WaitFrames 2;$after=Snapshot
    $arm.after=$after;$arm.inputMilliseconds=$clock.Elapsed.TotalMilliseconds
    Require ($after.frame-gt$before.frame -and $after.location.revision-gt$before.location.revision) 'Free-flight tick/revision did not advance'
    Require ($after.location.revision-le$before.location.revision+8) 'Unbounded origin churn during short input arm'
    $continuity=CheckContinuity $before $after $maximumMovement
    $arm.continuity=$continuity
    $retired=Ground $before.camera.position[0] $before.camera.position[2]
    $arm.retiredCoordinates=$retired
    Require ($retired.revision-eq$after.location.revision -and !$retired.covered) 'Old-frame coordinates retained stale resident support'
    Require ($retired.generation-eq0 -or $retired.generation-gt$oldGround.generation) 'Old resident generation survived the origin change'
    return $arm
}
try{
    foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    $null=New-Item -ItemType Directory -Force -Path $env:POSEIDON_USER_DIR
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','20000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$world+'"'),'--test-world-freefly','6400','6400','1000','0','-20','--test-world-hour','12','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $result.ownedPID=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(150)
    do{
        Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
        if(!$client){Require ([DateTime]::UtcNow-lt$until) 'Harness startup deadline';Start-Sleep -Milliseconds 250}
    }while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=15000;$stream.WriteTimeout=15000
    $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    while((Eval 'triSceneReady')-cne'OK'){Require ([DateTime]::UtcNow-lt$until) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
    Require (!(Select-String -LiteralPath $log -Pattern 'Cannot load --test-world|Critical error' -Quiet)) 'Fixture world failed to load'
    $null=Send @{cmd='exec';code='setAccTime 1;player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;triDevPanelShow 0;triCursorMove [0,0]'}
    $result.initial=WaitGround (Location)
    $near=RebaseArm 'near-1000m' 1000 32000 5000
    $near.fresh=WaitGround $near.after.location $result.initial.ground.generation
    $near.screenshot=Capture 'near-rebased'
    $relocated=@(Select-String -LiteralPath $log -Pattern ('Earth relocated: .*revision '+$near.after.location.revision+'$'))
    Require ($relocated.Count-gt0) 'Near renderer rebase receipt missing'
    Require (!(@(PatchRows)|Where-Object {$_.lineNumber-gt$relocated[-1].LineNumber -and $_.revision-ne$near.after.location.revision})) 'Superseded near terrain published after rebase'
    # The orbital case crosses the antimeridian without requesting another DEM
    # region: DrawEarthTerrain already omits DEM entirely above 500 km altitude.
    Require ((Eval 'triEarthLocation [0,179.5,18000000]').StartsWith('OK:')) 'Orbital geographic setup refused'
    $orbitLocation=Location;WaitFrames 2
    $cleared=Ground $anchor $anchor;Require (!$cleared.covered -and $cleared.generation-eq0 -and $cleared.revision-eq$orbitLocation.revision) 'Orbital Go retained old near-ground support'
    $orbit=RebaseArm 'orbital-dateline-18000000m' 18000000 400000 2000000
    Require ($orbit.before.location.longitude-gt179 -and $orbit.after.location.longitude-lt0) 'Automatic orbital rebase did not wrap the dateline'
    Require (!(@(PatchRows)|Where-Object {$_.revision-ge$orbitLocation.revision})) 'Orbital arm unexpectedly requested/published near DEM'
    $orbit.screenshot=Capture 'orbital-dateline-rebased'
    $result.performance=Send @{cmd='frame_performance'}
    $tiles=@(Get-ChildItem -LiteralPath $CacheDirectory -Directory -Filter 'terrarium-z*'|Get-ChildItem -Filter '*.png')
    Require ($tiles.Count-le256 -and !($tiles|Where-Object {$_.Length-gt2MB})) 'Elevation disk cache bound exceeded'
    $result.elevationTiles=$tiles.Count
    if($Offline){Require (!(Select-String -LiteralPath $log -Pattern 'downloads [1-9]' -Quiet)) 'Offline run unexpectedly downloaded elevation'}
    Require (!(Select-String -LiteralPath @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error' -Quiet)) 'Runtime error'
    Require ((Get-Content -LiteralPath $stampPath -Raw).Trim()-ceq$stamp) 'Installed build changed during acceptance'
    $result.passed=$true;Write-Host ('Earth geodesic installed acceptance PASS: '+$out)
}catch{$result.error=$_.Exception.Message;throw}finally{
    if($p -and !$p.HasExited){
        if($writer){try{if($null-ne$heldKey){$writer.WriteLine('{"cmd":"key_up","sc":7}')};$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}}
        if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}
    }
    if($client){$client.Dispose()}
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $out 'result.json')
}
