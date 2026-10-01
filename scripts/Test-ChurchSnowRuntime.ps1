# Targeted original Eden Church roof captures. Root owns installed execution.
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
    [ValidateRange(0.04,0.5)][double]$Depth=0.18,
    [ValidateRange(1,10)][int]$SettleSeconds=2,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-church',
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Number($value){$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite number.';return $n}
function Model-Key([string]$name){return $name.Replace('\','/').ToLowerInvariant()}
function Pose($values){Require ($values.Count -eq 5) 'Five camera coordinates required.';return (@($values|ForEach-Object{(Number $_).ToString('R',$culture)}) -join ' ')}
function Decode-Eval([string]$value){
    $value=$value.Trim()
    if($value.StartsWith('"')){Require ($value.Length -ge 2 -and $value.EndsWith('"')) 'Invalid SQF string.';return $value.Substring(1,$value.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $value
}
function Restore-Environment([string]$key,$value){
    # .NET10 retains an empty variable when SetEnvironmentVariable binds null.
    if($null -eq $value){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}
    else{[Environment]::SetEnvironmentVariable($key,$value,'Process')}
}
function Assert-Owner($owner,$fixture,$previous){
    Require ($null -ne $owner -and $owner.id -eq $fixture.id -and $owner.present -is [bool] -and $owner.present) 'Actual source owner absent.'
    Require ($owner.destroyed -is [bool] -and !$owner.destroyed -and $owner.destroyPhase -eq 0 -and $owner.rawDamage -eq 0) 'Church owner damaged.'
    Require ((Model-Key $owner.model) -ceq (Model-Key $fixture.model)) 'Actual model differs from audited original map owner.'
    Require ($owner.visualResident -is [bool] -and $owner.visualResident) 'Actual visual owner not resident.'
    Require ($owner.hasGeometry -is [bool] -and $owner.hasGeometry -and $owner.hasViewGeometry -is [bool] -and $owner.hasViewGeometry) 'Actual collision/shelter geometry absent.'
    Require ($owner.normalVertexBuffers -ge 0) 'Missing geometry-buffer diagnostic.'
    Require ($owner.frame.Count -eq 12 -and (Number $owner.radius) -gt 0) 'Actual owner transform/radius missing.'
    foreach($value in @($owner.x,$owner.y,$owner.z)+@($owner.frame)){$null=Number $value}
    # WRP tool prints decimetres. Live world position is authoritative thereafter.
    for($i=0;$i -lt 3;++$i){Require ([Math]::Abs(@($owner.x,$owner.y,$owner.z)[$i]-$fixture.position[$i]) -lt 1) 'World ID does not bind audited WRP placement.'}
    if($previous){for($i=0;$i -lt 12;++$i){Require ([Math]::Abs([double]$owner.frame[$i]-[double]$previous.frame[$i]) -lt 0.001) 'Static owner frame changed.'}}
}
function Church-Trace($lines,[string]$model){
    $retained=@();$clocks=@();$fixed=@();$maps=@();$uploads=@();$off=@()
    foreach($line in $lines){
        if($line.Contains('Wgpu object snow retained gates:')){
            if($line -match 'model=(.+?) eligible=true .+poseAllowed=true\s*$' -and (Model-Key $Matches[1]) -ceq (Model-Key $model)){$retained+=$line}
        }elseif($line.Contains('Wgpu object snow direct gates:')){
            if($line -match 'model=(.+?) texture=(.+?) sections=' -and (Model-Key $Matches[1]) -ceq (Model-Key $model)){
                $texture=$Matches[2]
                if($texture -match 'hodiny_ruc'){
                    Require ($line -match 'sourceEligible=false' -and $line -match 'churchFixedSection=false\s*$') 'Moving clock geometry received fixed roof admission.'
                    $clocks+=$line
                }elseif($line -match 'churchFixedSection=true\s*$'){$fixed+=$line}
            }
        }elseif($line.Contains('[wgr] weather cover map:')){
            if($line -match 'ready=true submitted=true scope=registered-retained-and-current-direct-no-all-world-dynamic-proof\s*$'){$maps+=$line}
        }elseif($line.Contains('wgpu object snow: enabled=')){
            Require ($line -match 'enabled=([01]) deposit=([-+0-9.eE]+) snowlineHeight=([-+0-9.eE]+) snowlineRange=([-+0-9.eE]+) snowlineDepth=([-+0-9.eE]+)\s*$') 'Unknown object-snow upload grammar.'
            $m=$Matches.Clone();$deposit=Number $m[2]
            if((Number $m[3]) -lt 0 -and (Number $m[4]) -eq 0 -and (Number $m[5]) -eq 0){
                if($m[1] -eq '1' -and [Math]::Abs($deposit-$Depth) -le .000051){$uploads+=$line}
                if($m[1] -eq '0' -and $deposit -eq 0){$off+=$line}
            }
        }
    }
    return @{retained=@($retained|Select-Object -Unique);clockDenied=@($clocks|Select-Object -Unique);fixedDirect=@($fixed|Select-Object -Unique);readyMaps=@($maps);depositUploads=@($uploads);disabledUploads=@($off)}
}
function Transform-Point($frame,$point){
    Require ($frame.Count -eq 12 -and $point.Count -eq 3) 'Source point/frame dimensions differ.'
    return @(for($axis=0;$axis -lt 3;++$axis){(Number $frame[9+$axis])+(Number $frame[$axis])*(Number $point[0])+(Number $frame[3+$axis])*(Number $point[1])+(Number $frame[6+$axis])*(Number $point[2])})
}
function Clock-Eye($clock,$frame){
    return @(($clock[0]-18*$frame[6]),($clock[1]+1),($clock[2]-18*$frame[8]))
}
function Aim-Pose($eye,$target){
    $dx=$target[0]-$eye[0];$dy=$target[1]-$eye[1];$dz=$target[2]-$eye[2];$horizontal=[Math]::Sqrt($dx*$dx+$dz*$dz)
    Require ($horizontal -gt .001) 'View target directly vertical or degenerate.'
    return Pose @($eye[0],$eye[2],$eye[1],([Math]::Atan2($dx,$dz)*180/[Math]::PI),([Math]::Atan2($dy,$horizontal)*180/[Math]::PI))
}
function Assert-Snow($state,[bool]$enabled,$chunks){
    Require ($state.enabled -is [bool] -and $state.enabled -eq $enabled -and $state.falling -is [bool] -and !$state.falling -and $state.geometry -is [bool] -and $state.geometry) 'Fixed deposited snow mode differs.'
    Require ([Math]::Abs((Number $state.depth)-$Depth) -lt 0.00001 -and $null -ne $state.chunks -and (Number $state.chunks) -ge 0 -and $state.chunks -eq [Math]::Floor([double]$state.chunks)) 'Actual deposited snow differs.'
    if($null -ne $chunks){Require ($state.chunks -eq $chunks) 'Camera/switch changed stored snow chunks.'}
}
function Assert-Pair($first,$last){Require (($first|ConvertTo-Json -Depth 5 -Compress) -ceq ($last|ConvertTo-Json -Depth 5 -Compress)) 'Installed EXE/DLL stamp/hash pair changed.'}
function Installed-State{
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    Require ($stamp -match '^codex/streaming-residency\s+([a-fA-F0-9]{8,40})\s+') 'Installed branch/commit stamp missing.'
    $installedCommit=$Matches[1];$common=[Math]::Min($installedCommit.Length,$ExpectedCommit.Length)
    Require ($common -ge 8 -and $installedCommit.Substring(0,$common).Equals($ExpectedCommit.Substring(0,$common),[StringComparison]::OrdinalIgnoreCase)) 'Installed commit differs from requested pair.'
    $files=@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$file=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length;writtenUtc=$file.LastWriteTimeUtc.ToString('o')}}
    return [ordered]@{stamp=$stamp;files=@($files)}
}
function Test-Helpers{
    $fixture=@{id=4867;model='data3d\kostel3.p3d';position=@(5056.6,32.9,3920)}
    $owner=[pscustomobject]@{id=4867;present=$true;destroyed=$false;destroyPhase=0;rawDamage=0;model=$fixture.model;visualResident=$true;hasGeometry=$true;hasViewGeometry=$true;normalVertexBuffers=0;radius=26;x=5056.6;y=32.9;z=3920;frame=@(1,0,0,0,1,0,0,0,1,5056.6,32.9,3920)}
    Assert-Owner $owner $fixture $owner
    $point=Transform-Point $owner.frame @(0,-7,9);Require ([Math]::Abs($point[1]-25.9) -lt .0001 -and $point[2] -eq 3929) 'Column-major live frame point transform failed.'
    $rotated=Transform-Point @(0,0,-1,0,1,0,1,0,0,10,20,30) @(2,3,4)
    Require (($rotated -join ',') -ceq '14,23,28') 'Rotated live owner frame used an incorrect axis convention.'
    Require (((Clock-Eye @(14,23,28) @(0,0,-1,0,1,0,1,0,0,10,20,30)) -join ',') -ceq '-4,24,28') 'Clock camera must produce three scalar coordinates for a rotated frame.'
    Require ((Aim-Pose @(5056.6,30,3860) @(5056.6,30,3920)) -ceq '5056.6 3860 30 0 0') 'Heading/height camera order wrong.'
    $line='Wgpu object snow retained gates: model=data3d\kostel3.p3d eligible=true static=true poseAllowed=true'
    Require ((Church-Trace @($line) $fixture.model).retained.Count -eq 1) 'Retained proof parser failed.'
    $clock='Wgpu object snow direct gates: model=data3d\kostel3.p3d texture=data\k2_hodiny_ruc_mala.paa sections=1:2 sourceEligible=false churchFixedSection=false'
    Require ((Church-Trace @($clock) $fixture.model).clockDenied.Count -eq 1) 'Clock negative trace absent.'
    $failed=$false;try{Church-Trace @($clock.Replace('churchFixedSection=false','churchFixedSection=true')) $fixture.model|Out-Null}catch{$failed=$true};Require $failed 'Clock coat admission accepted.'
    $failed=$false;try{Number 'NaN'|Out-Null}catch{$failed=$true};Require $failed 'Nonfinite input accepted.'
    $upload='wgpu object snow: enabled=1 deposit='+$Depth.ToString('F4',$culture)+' snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000'
    Require ((Church-Trace @($upload) $fixture.model).depositUploads.Count -eq 1) 'Actual deposited object-snow upload was not recognized.'
    Require ((Church-Trace @('wgpu object snow: enabled=0 deposit=0.0000 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000') $fixture.model).disabledUploads.Count -eq 1) 'Master-off zero upload was not recognized.'
    $owner.model='data3d\kostel.p3d';$failed=$false;try{Assert-Owner $owner $fixture $null}catch{$failed=$true};Require $failed 'Wrong church model accepted.'
    Write-Host 'Church fixture frame/camera/source-trace falsifiers passed. No game/GPU/profile used.'
}
if($SelfTest){Test-Helpers;return}
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit for the installed pair.'
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game has another owner.'
$before=Installed-State
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
$fixture=@{id=4867;model='data3d\kostel3.p3d';position=@(5056.6,32.9,3920);mission='tests/perf/missions/perf_field.eden';wrpSha256='5A2550B27612FC4C8CCDEC55CE2555C1ED23114D46F8B687CE5FB8B273515A79';source='Original installed Worlds/eden.wrp; source census owner4867 rounded decimetres.'}
Require ((Get-FileHash -LiteralPath (Join-Path $GameDir 'Worlds/eden.wrp')).Hash -ceq $fixture.wrpSha256) 'Original Eden corpus hash changed.'
$output=Join-Path $root ('build/church-snow/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $output -Force|Out-Null
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;installed=$before;expectedCommit=$ExpectedCommit;lockOwner=$env:LOCK_OWNER;arms=@{};error=$null;
    scope='Actual original Eden Church owner/frame/residency, fixed-roof and clock-negative source traces, deposited state, paused ON/OFF/master-off/return captures. Interior/clock pixels require inspected visibility; no automatic appearance acceptance.'}
$result.helperSha256=@{};foreach($helper in @('Write-BenchmarkGraphics.ps1','Assert-CaptureLifecycle.ps1','TerrainPuddleStockRoof.ps1')){$result.helperSha256[$helper]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $helper)).Hash}
$settings=@{POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';WGR_OBJECT_SNOW='1';WGR_TREE_SNOW='1';WGR_TREE_SNOW_TRACE='0';WGR_FOREST_SNOW_ATLAS='1';WGR_SNOW_SURFACE_FIXTURE='1';WGR_GPU_DRIVEN='1';WGR_SNOW_POWDER='1';WGR_FOREST_SNOW_COVER_DEBUG='0';
    WGR_WEATHER_COVER_FAR='1';
    WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_JITTER='0';
    WGR_INTERIOR_SKY='1';WGR_INTERIOR_SKY_DEBUG='0';WGR_TERRAIN_PUDDLES='0';WGR_GROUND_PUDDLES='0';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_TRACE='1'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
# Normal engine grade; explicit stable cloud/wind/LOD controls remain.
$settings.Remove('WGR_EXPOSURE');$settings.Remove('WGR_AUTO_EXPOSURE')
$clear+=@('WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE')
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR');$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result.exposureMode='normal engine time-of-day grade and default adaptive policy'
$result.environment=@{settings=$settings;cleared=$clear;objectArms=@('1','0');inherited=$saved}
$p=$null;$client=$null;$reader=$null;$writer=$null;$log=$null;$armDir=$null;$armDeadline=$null
function Live-Lines([string]$path){
    if(!(Test-Path -LiteralPath $path)){return @()}
    $stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$r=[IO.StreamReader]::new($stream)
    try{$text=$r.ReadToEnd();$end=$text.LastIndexOf("`n");if($end -ge 0){return $text.Substring(0,$end).Split("`n")};return @()}finally{$r.Dispose()}
}
function Logs{return @(@($log,$stdout,$stderr)|ForEach-Object{Live-Lines $_})}
function Health{Require (!$p.HasExited) 'Owned game exited early.';Require ([DateTime]::UtcNow -lt $armDeadline) 'Owned arm exceeded bounded600s.';Require (!(Logs|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Cannot load --test-world|StartAutoTest could not boot')) 'Runtime failure in capture logs.'}
function Send($request){Health;$json=$request|ConvertTo-Json -Compress;$json|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$writer.WriteLine($json)
    $replyDeadline=[DateTime]::UtcNow.AddSeconds(30)
    do{Require ([DateTime]::UtcNow -lt $replyDeadline) 'Harness response exceeded30s.';$line=$reader.ReadLine();Require ($null -ne $line) 'Harness closed.';$line|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
    Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Owner($fixture,$previous){$reply=Send @{cmd='stream_identity_probe';ids=@($fixture.id)};Require ($reply.objects.Count -eq 1) 'No unique actual owner.';Assert-Owner $reply.objects[0] $fixture $previous;return $reply.objects[0]}
function Capture([string]$name,[string]$pose,$fixture,$target,[bool]$enabled,$chunks){
    Require ((Eval ('triFreeFlyPose "'+$pose+'"')) -ceq 'OK') 'Camera refused.';Start-Sleep -Seconds $SettleSeconds
    $actual=Owner $fixture $target;Require ([Math]::Abs([double](Eval 'time')-100) -lt 0.001) 'Paused time changed.'
    $state=Send @{cmd='dev_snow';action='state'};Assert-Snow $state $enabled $chunks
    $weather=Send @{cmd='weather_visibility'};foreach($field in @('rain','fog','liquidRain','particleDensity')){Require ($null -ne $weather.$field -and (Number $weather.$field) -le 0.001) "Fixed dry weather differs: $field"}
    Require ($weather.particleSnowflakes -is [bool] -and !$weather.particleSnowflakes) 'Particle snow override present.'
    $path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        Health
        if (Test-Path -LiteralPath $path) {
            # The renderer can create the path before finishing its write.
            # Retry a sharing violation until the same bounded PNG deadline.
            try {$bytes=[IO.File]::ReadAllBytes($path)} catch [IO.IOException] {$bytes=$null}
            if ($bytes -and $bytes.Length -ge 36 -and ($bytes[($bytes.Length-8)..($bytes.Length-5)] -join ',') -eq '73,69,78,68') {break}
        }
        Require ([DateTime]::UtcNow -lt $deadline) 'Complete screenshot absent.'
        Start-Sleep -Milliseconds 100
    } while ($true)
    Require ([BitConverter]::ToString($bytes[0..7]) -ceq '89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG.'
    Require (($bytes[16..19] -join ',') -eq '0,0,5,0' -and ($bytes[20..23] -join ',') -eq '0,0,2,208') 'Capture dimensions differ from 1280x720.'
    return @{path=$path;pose=$pose;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;owner=$actual;snow=$state;weather=$weather;trace=(Church-Trace (Logs) $fixture.model)}
}
try{
    foreach($key in $clear){Restore-Environment $key $null};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    $audit=Join-Path $output 'stock-source'
    & python (Join-Path $PSScriptRoot 'Inspect-ChurchSnowStockSource.py') --game-dir $GameDir --out $audit
    Require ($LASTEXITCODE -eq 0) 'Actual stock church source audit failed.'
    $source=Get-Content (Join-Path $audit 'source-proof.json') -Raw|ConvertFrom-Json
    $result.stockSource=$source
    $paired=$null;$views=$null;$sourceTargets=$null;$mission=Join-Path $root $fixture.mission;Require (Test-Path -LiteralPath (Join-Path $mission 'mission.sqm')) 'Original-map authored mission missing.'
    foreach($objectFlag in @('1','0')){
            Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.';Assert-Pair $before (Installed-State)
            $armName='Eden-object'+$objectFlag;$armDir=Join-Path $output $armName;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Path $profile -Force|Out-Null
            & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
            $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;').Replace('fpsCap=0;','fpsCap=60;')))
            $env:POSEIDON_USER_DIR=$profile;$env:WGR_OBJECT_SNOW=$objectFlag;$log=Join-Path $armDir 'engine.log';$stdout=Join-Path $armDir 'stdout.txt';$stderr=Join-Path $armDir 'stderr.txt'
            $seed=Pose @($fixture.position[0],($fixture.position[2]-60),($fixture.position[1]+35),0,-20)
            $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
            $argsList=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly')+($seed -split '\s+')+@('--log-file',('"'+$log+'"'))
            $arm=@{status='starting';fixture=$fixture;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;objectSnow=$objectFlag;arguments=$argsList;log=$log;stdout=$stdout;stderr=$stderr;profileSha256=(Get-FileHash -LiteralPath $cfg).Hash;captures=@{};before=(Installed-State)};$result.arms[$armName]=$arm
            $armDeadline=[DateTime]::UtcNow.AddSeconds(600)
            $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $argsList -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru;$null=$p.Handle;$arm.pid=$p.Id
            $deadline=[DateTime]::UtcNow.AddSeconds(120)
            do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $deadline) 'Harness startup exceeded120s.';Start-Sleep -Milliseconds 250}}while(!$client)
            $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
            Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Original map scene not ready.'
            $null=Send @{cmd='exec';code='player allowDamage false;0 setFog 0;0 setOvercast 0;0 setRain 0;setAccTime 0;setDate [1985,6,21,16,0]'}
            Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Time freeze refused.';Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Brightness refused.'
            $null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
            $initial=Send @{cmd='dev_snow';action='state'};Require ((Number $initial.depth) -eq 0 -and $initial.chunks -eq 0) 'Initial snow state not empty.'
            $null=Send @{cmd='dev_snow';action='enable'};$null=Send @{cmd='dev_snow';action='deposit';metres=$Depth};$snow=Send @{cmd='dev_snow';action='state'};Assert-Snow $snow $true $null
            $target=Owner $fixture $paired;if(!$paired){$paired=$target;$views=[ordered]@{}
                # Actual current frame transforms audited source targets. Source
                # local positions are candidate image aims, not world shelter proof.
                $roof=Transform-Point $target.frame $source.visualLods[0].roofLocalCentre
                $clock=Transform-Point $target.frame $source.visualLods[0].clockLocalCentre
                $sourceTargets=@{roof=$roof;clock=$clock;scope='Actual source local targets transformed by live owner frame; image visibility still requires inspection'}
                foreach($distance in @(30,200,500)){
                    $cx=$roof[0];$cz=$roof[2]-$distance;$ground=Get-TerrainPuddleFixtureHeight {param($request) Send $request} $cx $cz
                    $eye=[Math]::Max($ground+8,$roof[1]+.18*$distance)
                    $views['distance'+$distance]=Aim-Pose @($cx,$eye,$cz) $roof
                }
                $views.top=Aim-Pose @($roof[0],($target.y+[Math]::Max(60,2*$target.radius)),($roof[2]-3)) $roof
                $eye=Clock-Eye $clock $target.frame
                $views.clock=Aim-Pose $eye $clock
                $underGround=Get-TerrainPuddleFixtureHeight {param($request) Send $request} $roof[0] $roof[2]
                $views.interiorCandidate=Aim-Pose @($roof[0],($underGround+1),($roof[2]-1)) $roof
                $views.originalScene='5025 3725 144.545459747314 0 -8.53076560994813'
                $views.returnNear=$views.distance30
            };$arm.target=$target;$arm.views=$views;$arm.sourceTargets=$sourceTargets
            foreach($view in $views.Keys){$arm.captures['enabled-'+$view]=Capture ('enabled-'+$view) $views[$view] $fixture $target $true $snow.chunks}
            $trace=Church-Trace (Logs) $fixture.model
            Require ($trace.retained.Count -gt 0) 'Actual fixed church retained source admission absent.'
            Require ($trace.clockDenied.Count -gt 0) 'Near clock negative source trace absent; inspect whether view selected the clock geometry.'
            if($objectFlag -eq '1'){
                Require ($trace.readyMaps.Count -gt 0) 'Ready physical weather map trace absent.'
                Require ($trace.depositUploads.Count -gt 0) 'Actual raised snow upload absent.'
            }else{
                Require ($trace.disabledUploads.Count -gt 0 -and $trace.depositUploads.Count -eq 0) 'Object-master-off upload is not actually zero.'
                Require ($trace.readyMaps.Count -eq 0) 'Inactive dry object-snow-off arm unexpectedly published weather cover.'
            }
            $arm.trace=$trace
            $null=Send @{cmd='dev_snow';action='disable'}
            foreach($view in @('distance30','distance200','distance500','top','clock','interiorCandidate')){$arm.captures['disabled-'+$view]=Capture ('disabled-'+$view) $views[$view] $fixture $target $false $snow.chunks}
            $null=Send @{cmd='dev_snow';action='enable'}
            foreach($view in @('distance30','distance200','distance500','top')){$arm.captures['restored-'+$view]=Capture ('restored-'+$view) $views[$view] $fixture $target $true $snow.chunks}
            $null=Send @{cmd='exec';code='setAccTime 1'};$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal exit failed.'
            Require ((Logs|Select-String 'Shutdown complete') -and !(Logs|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION')) 'Normal exit/validation log gate failed.'
            $arm.lifecycle=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$meta=Join-Path $armDir 'lifecycle.json';$arm.lifecycle|ConvertTo-Json|Set-Content -LiteralPath $meta
            & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $meta -LogPath $log
            $arm.after=Installed-State;Assert-Pair $before $arm.after;$arm.status='captured-not-visually-accepted';$client.Dispose();$client=$null;$p=$null
    }
    $result.status='captured-not-visually-accepted'
}catch{$result.status='failed';$result.error=$_.Exception.Message;throw}
finally{
    if ($p -and !$p.HasExited -and $client -and $writer -and $reader) {
        try {
            # Even a failed capture should release its owned session normally.
            $writer.WriteLine('{"cmd":"exit"}');$null=$reader.ReadLine()
            $null=$p.WaitForExit(20000)
        } catch {$result.cleanupError=$_.Exception.Message}
    }
    if($client){$client.Dispose()}
    if($p -and !$p.HasExited){# Own PID only. A forced exit can never produce accepted lifecycle metadata.
        $result.status='failed';$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$null=$p.WaitForExit(10000)
    }
    foreach($key in $keys){Restore-Environment $key $saved[$key]}
    $result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Church capture record: $output"
}
