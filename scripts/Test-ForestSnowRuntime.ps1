# Source-proven stock forest captures. Run through with-game-lock.sh; no deployment.
[CmdletBinding()]
param(
    [switch]$CoverageDebug,
    [switch]$NormalExposure,
    [switch]$FarCover,
    [switch]$DefaultFarCover,
    [ValidateSet('All','Eden','Noe')][string]$World='All',
    [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
    [ValidateRange(0.04,0.5)][double]$Depth=0.18,
    [ValidateRange(1,10)][int]$SettleSeconds=2,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-forest',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
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
    Require ($owner.destroyed -is [bool] -and !$owner.destroyed -and $owner.destroyPhase -eq 0 -and $owner.rawDamage -eq 0) 'Forest owner damaged.'
    Require ((Model-Key $owner.model) -ceq (Model-Key $fixture.model)) 'Actual model differs from audited original map owner.'
    Require ($owner.visualResident -is [bool] -and $owner.visualResident -and $owner.normalVertexBuffers -gt 0) 'Actual normal visual geometry not resident.'
    Require ($owner.frame.Count -eq 12 -and (Number $owner.radius) -gt 0) 'Actual owner transform/radius missing.'
    foreach($value in @($owner.x,$owner.y,$owner.z)+@($owner.frame)){$null=Number $value}
    # WRP tool prints decimetres. Live world position is authoritative thereafter.
    for($i=0;$i -lt 3;++$i){Require ([Math]::Abs(@($owner.x,$owner.y,$owner.z)[$i]-$fixture.position[$i]) -lt 1) 'World ID does not bind audited WRP placement.'}
    if($previous){for($i=0;$i -lt 12;++$i){Require ([Math]::Abs([double]$owner.frame[$i]-[double]$previous.frame[$i]) -lt 0.001) 'Static owner frame changed.'}}
}
function Forest-Trace($lines,[string]$model){
    $owners=@();$atlases=@();$maps=@();$uploads=@()
    foreach($line in $lines){
        if($line.Contains('FOREST_SNOW_OWNER model=')){
            Require ($line -match 'FOREST_SNOW_OWNER model=(.+?) mode=([01]) proof=2 scope=primary-static-known-ForestPlain-owned-mesh\s*$') 'Unknown owner trace grammar.'
            if((Model-Key $Matches[1]) -ceq (Model-Key $model)){$owners+=$line}
        }elseif($line.Contains('FOREST_SNOW_ATLAS model=')){
            Require ($line -match 'FOREST_SNOW_ATLAS model=(.+?) lod=(\d+) resolution=([-+0-9.eE]+) texture=(.+?) category=(\d+) alphaRef=([-+0-9.eE]+) scope=audited-uv-crowns-map-only\s*$') 'Unknown atlas registration grammar.'
            $m=$Matches.Clone();$res=Number $m[3];$alpha=Number $m[6]
            Require ($res -ge 0 -and [int]$m[5] -ge 1 -and [int]$m[5] -le 15 -and $alpha -gt 0 -and $alpha -le 1) 'Invalid admitted material registration.'
            if((Model-Key $m[1]) -ceq (Model-Key $model)){$atlases+=@{model=$m[1];loadedSlot=[int]$m[2];resolution=$res;texture=$m[4];category=[int]$m[5];alphaRef=$alpha;line=$line}}
        }elseif($line.Contains('[wgr] weather cover map:')){
            if($line -match 'resolution=2048 extent=640 .+ready=true submitted=true scope=registered-retained-and-current-direct-no-all-world-dynamic-proof\s*$'){$maps+=$line}
        }elseif($line.Contains('wgpu object snow: enabled=1 deposit=')){
            Require ($line -match 'wgpu object snow: enabled=1 deposit=([-+0-9.eE]+) snowlineHeight=([-+0-9.eE]+) snowlineRange=([-+0-9.eE]+) snowlineDepth=([-+0-9.eE]+)\s*$') 'Unknown snow upload grammar.'
            $m=$Matches.Clone();if([Math]::Abs((Number $m[1])-$Depth) -le 0.000051 -and (Number $m[2]) -lt 0 -and (Number $m[3]) -eq 0 -and (Number $m[4]) -eq 0){$uploads+=$line}
        }
    }
    return @{owners=@($owners|Select-Object -Unique);atlases=@($atlases);readyMaps=@($maps);depositUploads=@($uploads)}
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
    $script:checks=0
    function Check([bool]$ok){Require $ok 'Self-test failed.';++$script:checks}
    function Refuses([scriptblock]$action){$failed=$false;try{& $action|Out-Null}catch{$failed=$true};Check $failed}
    $fixture=@{id=4043;model='data3d\les trojuhelnik pruchozi.p3d';position=@(5025,57.5,4225)}
    $owner=[pscustomobject]@{id=4043;present=$true;destroyed=$false;destroyPhase=0;rawDamage=0;model=$fixture.model;visualResident=$true;normalVertexBuffers=3;radius=36;x=5025;y=57.5;z=4225;frame=@(1,0,0,0,1,0,0,0,1,5025,57.5,4225)}
    Assert-Owner $owner $fixture $owner;Check $true
    $owner.normalVertexBuffers=0;Refuses {Assert-Owner $owner $fixture $null};$owner.normalVertexBuffers=3
    $owner.model='data3d\les ctverec.p3d';Refuses {Assert-Owner $owner $fixture $null};$owner.model=$fixture.model
    $owner.frame[0]=[double]::NaN;Refuses {Assert-Owner $owner $fixture $null};$owner.frame[0]=1
    $o='FOREST_SNOW_OWNER model='+$fixture.model+' mode=1 proof=2 scope=primary-static-known-ForestPlain-owned-mesh'
    $a='FOREST_SNOW_ATLAS model='+$fixture.model+' lod=2 resolution=12 texture=merged\00001&krovi4.paa category=1 alphaRef=0.5 scope=audited-uv-crowns-map-only'
    $t=Forest-Trace @($o,$a) $fixture.model;Check ($t.owners.Count -eq 1 -and $t.atlases.Count -eq 1 -and $t.atlases[0].loadedSlot -eq 2)
    Check ((Forest-Trace @($o,$a) 'unrelated.p3d').atlases.Count -eq 0)
    Refuses {Forest-Trace @($a.Replace('alphaRef=0.5','alphaRef=0')) $fixture.model}
    Refuses {Forest-Trace @($a.Replace('category=1','category=16')) $fixture.model}
    Refuses {Forest-Trace @($o.Replace('mode=1','mode=2')) $fixture.model}
    $map='[wgr] weather cover map: frame=12 instanceEpoch=2 image=1 resolution=2048 extent=640 retained=3 direct=0 ready=true submitted=true scope=registered-retained-and-current-direct-no-all-world-dynamic-proof'
    $upload='wgpu object snow: enabled=1 deposit='+$Depth.ToString('F4',$culture)+' snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000'
    $positive=Forest-Trace @($map,$upload) $fixture.model;Check ($positive.readyMaps.Count -eq 1 -and $positive.depositUploads.Count -eq 1)
    Check ((Forest-Trace @($map.Replace('ready=true','ready=false'),$upload.Replace('enabled=1','enabled=0')) $fixture.model).readyMaps.Count -eq 0)
    Check ((Forest-Trace @($upload.Replace('snowlineDepth=0.0000','snowlineDepth=0.1000')) $fixture.model).depositUploads.Count -eq 0)
    Refuses {Number 'NaN'};Refuses {Pose @(1,2,3)}
    Check ((Decode-Eval '"data3d\a b.p3d"') -ceq 'data3d\a b.p3d')
    $state=[pscustomobject]@{enabled=$true;falling=$false;geometry=$true;depth=$Depth;chunks=2};Assert-Snow $state $true 2;Check $true
    Refuses {Assert-Snow $state $false 2};Refuses {Assert-Snow $state $true 3}
    $old=[Threading.Thread]::CurrentThread.CurrentCulture
    try{[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE');Check ((Pose @(1.25,2.5,3.75,90,-86)) -ceq '1.25 2.5 3.75 90 -86')}finally{[Threading.Thread]::CurrentThread.CurrentCulture=$old}
    $key='POSEIDON_FOREST_RUNTIME_SELFTEST';$saved=[Environment]::GetEnvironmentVariable($key,'Process')
    try{[Environment]::SetEnvironmentVariable($key,'test','Process');Restore-Environment $key $null;Check ($null -eq [Environment]::GetEnvironmentVariable($key,'Process'))}finally{Restore-Environment $key $saved}
    Write-Host "Forest fixture helper checks passed: $checks. No game/GPU/profile used."
}
if($SelfTest){Test-Helpers;return}
Require (!($FarCover -and $DefaultFarCover)) 'Choose explicit far cover or the engine default policy.'
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit for the installed pair.'
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game has another owner.'
$before=Installed-State
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
$fixtures=@{
    Eden=@{id=4043;model='data3d\les trojuhelnik pruchozi.p3d';position=@(5025,57.5,4225);mission='tests/perf/missions/perf_field.eden';wrpSha256='5A2550B27612FC4C8CCDEC55CE2555C1ED23114D46F8B687CE5FB8B273515A79';source='Original installed Worlds/eden.wrp; offline tool placement, rounded decimetres.'}
    Noe=@{id=173643;model='o\tree\les_nw_jehl_t1.p3d';position=@(1626.3,132.9,10077);mission='tests/perf/missions/perf_sand.noe';wrpSha256='9B27B11661D3CB30F9F0749A9A4616BFC58D723C4750666D1E674DD4213598D2';source='Original Noe WRP from installed O.pbo; offline tool placement, rounded decimetres.'}
}
$output=Join-Path $root ('build/forest-snow/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $output -Force|Out-Null
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;installed=$before;expectedCommit=$ExpectedCommit;lockOwner=$env:LOCK_OWNER;arms=@{};error=$null;
    scope='Live original-map owner/frame/normal-buffer proof + loaded registration traces + deposited state + captures. Registration is not selected draw LOD; bounded map startup traces are not per-camera exposure proof. No semantic visual or timing acceptance.'}
$result.helperSha256=@{};foreach($helper in @('Write-BenchmarkGraphics.ps1','Assert-CaptureLifecycle.ps1','TerrainPuddleStockRoof.ps1')){$result.helperSha256[$helper]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $helper)).Hash}
$settings=@{POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';WGR_OBJECT_SNOW='1';WGR_TREE_SNOW_TRACE='1';WGR_FOREST_SNOW_ATLAS='1';WGR_SNOW_SURFACE_FIXTURE='1';WGR_GPU_DRIVEN='1';WGR_SNOW_POWDER='1';WGR_FOREST_SNOW_COVER_DEBUG=$(if($CoverageDebug){'1'}else{'0'});
    WGR_WEATHER_COVER_FAR=$(if($DefaultFarCover){$null}elseif($FarCover){'1'}else{'0'});
    WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_JITTER='0';
    WGR_INTERIOR_SKY='1';WGR_INTERIOR_SKY_DEBUG='0';WGR_TERRAIN_PUDDLES='0';WGR_GROUND_PUDDLES='0';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_TRACE='1'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
if ($NormalExposure) {
    $settings.Remove('WGR_EXPOSURE'); $settings.Remove('WGR_AUTO_EXPOSURE')
    $clear+=@('WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE')
}
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR','WGR_TREE_SNOW');$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result.exposureMode=$(if($NormalExposure){'normal engine time-of-day grade and default adaptive policy'}else{'manual exposure 1, adaptive off'})
$result.environment=@{settings=$settings;cleared=$clear;treeArms=@('1','0');inherited=$saved}
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
    return @{path=$path;pose=$pose;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;owner=$actual;snow=$state;weather=$weather;trace=(Forest-Trace (Logs) $fixture.model)}
}
try{
    foreach($key in $clear){Restore-Environment $key $null};foreach($key in $settings.Keys){Restore-Environment $key $settings[$key]}
    $worlds=if($World -eq 'All'){@('Eden','Noe')}else{@($World)}
    foreach($worldName in $worlds){
        $fixture=$fixtures[$worldName];$paired=$null;$views=$null;$mission=Join-Path $root $fixture.mission;Require (Test-Path -LiteralPath (Join-Path $mission 'mission.sqm')) 'Original-map authored mission missing.'
        foreach($treeFlag in @('1','0')){
            Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.';Assert-Pair $before (Installed-State)
            $armName=$worldName+'-tree'+$treeFlag;$armDir=Join-Path $output $armName;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Path $profile -Force|Out-Null
            & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
            $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;').Replace('fpsCap=0;','fpsCap=60;')))
            $env:POSEIDON_USER_DIR=$profile;$env:WGR_TREE_SNOW=$treeFlag;$log=Join-Path $armDir 'engine.log';$stdout=Join-Path $armDir 'stdout.txt';$stderr=Join-Path $armDir 'stderr.txt'
            $seed=Pose @($fixture.position[0],($fixture.position[2]-60),($fixture.position[1]+35),0,-20)
            $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
            $argsList=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly')+($seed -split '\s+')+@('--log-file',('"'+$log+'"'))
            $arm=@{status='starting';fixture=$fixture;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;treeFlag=$treeFlag;atlasFlag='1';arguments=$argsList;log=$log;stdout=$stdout;stderr=$stderr;profileSha256=(Get-FileHash -LiteralPath $cfg).Hash;captures=@{};before=(Installed-State)};$result.arms[$armName]=$arm
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
                # Origin/radius are live; no claimed crown bounding-box or centre.
                $aim=$target.y+[Math]::Min(12,0.3*$target.radius)
                foreach($distance in @(60,200,500)){$cx=$target.x;$cz=$target.z-$distance;$ground=Get-TerrainPuddleFixtureHeight {param($request) Send $request} $cx $cz;$eye=[Math]::Max($ground+8,$aim+0.15*$distance);$views['distance'+$distance]=Pose @($cx,$cz,$eye,0,([Math]::Atan2($aim-$eye,$distance)*180/[Math]::PI))}
                $views.top=Pose @($target.x,($target.z-5),($target.y+[Math]::Max(70,2*$target.radius)),0,-86)
                if($worldName -eq 'Eden'){$views.ownerScene='5018.51 4087.66 117.58 220.8 -86'}
                $views.returnNear=$views.distance60
            };$arm.target=$target;$arm.views=$views
            foreach($view in $views.Keys){$arm.captures['enabled-'+$view]=Capture ('enabled-'+$view) $views[$view] $fixture $target $true $snow.chunks}
            $trace=Forest-Trace (Logs) $fixture.model
            Require ($trace.owners.Count -gt 0 -and $trace.atlases.Count -gt 0) 'Actual audited forest owner/material registrations absent.'
            Require ($trace.readyMaps.Count -gt 0 -and $trace.depositUploads.Count -gt 0) 'Ready physical weather map or actual raised snow upload absent.';$arm.trace=$trace
            $null=Send @{cmd='dev_snow';action='disable'}
            foreach($view in @('distance60','distance500','top')){$arm.captures['disabled-'+$view]=Capture ('disabled-'+$view) $views[$view] $fixture $target $false $snow.chunks}
            $null=Send @{cmd='dev_snow';action='enable'}
            foreach($view in @('distance60','top')){$arm.captures['restored-'+$view]=Capture ('restored-'+$view) $views[$view] $fixture $target $true $snow.chunks}
            $null=Send @{cmd='exec';code='setAccTime 1'};$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal exit failed.'
            Require ((Logs|Select-String 'Shutdown complete') -and !(Logs|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION')) 'Normal exit/validation log gate failed.'
            $arm.lifecycle=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$meta=Join-Path $armDir 'lifecycle.json';$arm.lifecycle|ConvertTo-Json|Set-Content -LiteralPath $meta
            & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $meta -LogPath $log
            $arm.after=Installed-State;Assert-Pair $before $arm.after;$arm.status='captured-not-visually-accepted';$client.Dispose();$client=$null;$p=$null
        }
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
    $result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Forest capture record: $output"
}
