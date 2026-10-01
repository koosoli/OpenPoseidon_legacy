# Installed real-weather comparison; no forced wetness, water setter, or stamps.
[CmdletBinding()]
param(
    [ValidateSet('All','Baseline','SoilFilm','PhysicalSky','CombinedSky','CombinedSSR','Custom')][string]$SingleArm='All',
    [ValidateRange(0,1)][int]$Film=1,
    [ValidateRange(0,1)][int]$Water=1,
    [ValidateRange(0,1)][int]$SSR=1,
    [ValidateSet('noe','eden','abel','cain')][string]$World='noe',
    [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='wet-ground',
    [ValidateRange(30,600)][double]$RainSimSeconds=160,
    [ValidateRange(10,600)][double]$DrainSimSeconds=128,
    [ValidateRange(2000,12000)][int]$ViewDistance=2000,
    [double]$SearchX=2675,[double]$SearchZ=5125,
    [double]$TargetX=[double]::NaN,[double]$TargetZ=[double]::NaN,
    [double]$SoilX=4975,[double]$SoilZ=4675,
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Number($value){
    Require ($null -ne $value -and $value -isnot [bool]) 'Missing or boolean numeric value.'
    $n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture)
    Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite number.'
    return $n
}
function Pose($values){Require ($values.Count -eq 5) 'Five camera coordinates required.';return (@($values|ForEach-Object{(Number $_).ToString('R',$culture)}) -join ' ')}
function Restore-Environment([string]$key,$value){
    if($null -eq $value){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}
    else{[Environment]::SetEnvironmentVariable($key,$value,'Process')}
}
function Decode-Eval([string]$value){
    $value=$value.Trim()
    if($value.StartsWith('"')){Require ($value.Length -ge 2 -and $value.EndsWith('"')) 'Invalid SQF string.';return $value.Substring(1,$value.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $value
}
function Arm-Plan([string]$choice,[int]$film,[int]$water,[int]$ssr){
    $matrix=@(
        @{name='Baseline';film=0;water=0;ssr=0},
        @{name='SoilFilm';film=1;water=0;ssr=0},
        @{name='PhysicalSky';film=0;water=1;ssr=0},
        @{name='CombinedSky';film=1;water=1;ssr=0},
        @{name='CombinedSSR';film=1;water=1;ssr=1})
    if($choice -eq 'All'){return $matrix}
    if($choice -eq 'Custom'){Require ($film -in 0,1 -and $water -in 0,1 -and $ssr -in 0,1) 'Flags must be zero or one.';return @{name='Custom';film=$film;water=$water;ssr=$ssr}}
    $match=@($matrix|Where-Object {$_.name -eq $choice});Require ($match.Count -eq 1) 'Unknown arm.';return $match[0]
}
function Assert-Field($state){
    foreach($key in @('volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume','pendingSeconds','generation','revision','width','height','spacing')){$null=Number $state.$key}
    Require ($state.volume -ge 0 -and $state.rainVolume -ge 0 -and $state.infiltrationVolume -ge 0 -and $state.evaporationVolume -ge 0 -and $state.outletVolume -ge 0) 'Negative water budget.'
    $net=$state.rainVolume-$state.infiltrationVolume-$state.evaporationVolume-$state.outletVolume
    Require ([Math]::Abs($state.volume-$net) -lt [Math]::Max(1,$state.rainVolume*.001)) 'Installed water budget does not conserve volume.'
}
function Assert-Paused($before,$after){
    Assert-Field $before;Assert-Field $after
    foreach($key in @('volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume','pendingSeconds','generation','revision','width','height','spacing')){Require ($before.$key -eq $after.$key) "Paused field changed: $key"}
}
function Assert-Local($sample,[bool]$needWater){
    Require ($sample.valid -is [bool]) 'Local sample validity missing.'
    foreach($key in @('depth','height','terrainY','flowX','flowZ','revision')){$null=Number $sample.$key}
    Require ($sample.depth -ge 0) 'Negative local depth.'
    if($sample.valid){Require ([Math]::Abs($sample.height-$sample.depth-$sample.terrainY) -lt .15) 'Local water has no matching physical terrain support.'}
    if($needWater){Require ($sample.valid -and $sample.depth -gt .001) 'Selected wet target is locally absent.'}
}
function Assert-Soil($sample,[bool]$audited){
    foreach($key in @('x','z','surfaceY','surfaceDx','surfaceDz','offset','wetness')){$null=Number $sample.$key}
    Require ($sample.sourceEligible -is [bool]) 'Ground source eligibility missing.'
    if($audited){Require ($sample.world -ieq 'noe\noe.wrp' -and ([string]$sample.texture).Replace('\','/').ToLowerInvariant() -ceq 'o/pole2.paa' -and $sample.sourceEligible) 'Audited Noe pole2 mud source is not present.'}
}
function Renderer-Proof($lines,$arm){
    $weather=@();$grids=@();$policies=@();$ssrReady=@()
    foreach($line in $lines){
        if($line -match 'Wgpu terrain puddle weather: time=([-+0-9.eE]+) rain=([-+0-9.eE]+) accumulated=([-+0-9.eE]+) uploaded=([-+0-9.eE]+)'){
            $row=@{time=(Number $Matches[1]);rain=(Number $Matches[2]);accumulated=(Number $Matches[3]);uploaded=(Number $Matches[4]);line=$line}
            if($row.accumulated -gt .001){Require ($(if($arm.film -eq 0){$row.uploaded -eq 0}else{[Math]::Abs($row.uploaded-$row.accumulated) -le .00011})) 'Terrain upload differs from explicit soil+film flag.';$weather+=$row}
        }
        if($line -match 'Rain water grid: .*copied=(\d+) enabled=([01]) accepted=(true|false) maxDepth=([-+0-9.eE]+) independent=1'){
            Require ($Matches[3] -ceq 'true') 'Renderer rejected physical-water snapshot.'
            if([int]$Matches[1] -gt 0 -and $Matches[2] -ceq '1' -and (Number $Matches[4]) -gt 0){$grids+=$line}
        }
        if($line -match 'WGPU_RAIN_WATER_SSR_POLICY enabled=([01]) receiverRange=60 rayRange=25 steps=24 refine=4 scope=current-screen-world'){
            Require ([int]$Matches[1] -eq $arm.ssr) 'SSR policy differs from requested flag.';$policies+=$line
        }
        if($line -match 'WGPU_RAIN_WATER_SSR enabled=1 interest=1 ready=1 .*source=pre-rain-world selfFeedback=false'){$ssrReady+=$line}
    }
    Require ($weather.Count -gt 0) 'Actual terrain accumulation/upload diagnostic missing.'
    Require ($policies.Count -gt 0) 'Explicit SSR policy diagnostic missing.'
    Require (@($lines -match 'RAIN_WATER_INIT').Count -gt 0) 'Actual world water initialization missing.'
    if($arm.water -eq 1){Require ($grids.Count -gt 0) 'Positive physical-water upload acceptance missing.'}
    else{Require ($grids.Count -eq 0) 'Physical-water renderer remained active in OFF arm.'}
    if($arm.water -eq 1 -and $arm.ssr -eq 1){Require ($ssrReady.Count -gt 0) 'Enabled SSR never published a ready pre-rain source.'}
    return @{terrainWeather=$weather;positiveGridUploads=@($grids|Select-Object -Unique);ssrPolicy=@($policies|Select-Object -Unique);ssrReady=@($ssrReady|Select-Object -Unique);
        offProof='Explicit inherited-process flag provenance + absence of positive grid uploads; disabled Rust grid intentionally emits no positive upload row. This does not count visible fragments.'}
}
function Test-Helpers{
    $script:checks=0
    function Check([bool]$ok){Require $ok 'Self-test failed.';++$script:checks}
    function Refuses([scriptblock]$action){$failed=$false;try{& $action|Out-Null}catch{$failed=$true};Check $failed}
    $arms=@(Arm-Plan 'All' 0 0 0);Check ($arms.Count -eq 5 -and $arms[1].film -eq 1 -and $arms[1].water -eq 0 -and $arms[4].ssr -eq 1)
    Check ((Arm-Plan 'PhysicalSky' 1 0 1).water -eq 1);Check ((Arm-Plan 'physicalsky' 1 0 1).water -eq 1);Check ((Arm-Plan 'Custom' 0 1 1).film -eq 0)
    Refuses {Arm-Plan 'missing' 0 0 0};Refuses {Arm-Plan 'Custom' 2 0 0}
    $state=[pscustomobject]@{volume=80.;rainVolume=100.;infiltrationVolume=10.;evaporationVolume=5.;outletVolume=5.;pendingSeconds=0.;generation=1;revision=2;width=10;height=10;spacing=25.}
    Assert-Paused $state $state;Check $true
    $bad=$state.PSObject.Copy();$bad.revision=3;Refuses {Assert-Paused $state $bad}
    $bad=$state.PSObject.Copy();$bad.volume=20;Refuses {Assert-Field $bad}
    $sample=[pscustomobject]@{valid=$true;depth=.1;height=20.1;terrainY=20.;flowX=0.;flowZ=0.;revision=2}
    Assert-Local $sample $true;Check $true;$sample.height=21;Refuses {Assert-Local $sample $true};$sample.height=20.1
    $sample.depth=0;$sample.height=20;Assert-Local $sample $false;Check $true;Refuses {Assert-Local $sample $true}
    $soil=[pscustomobject]@{x=4975.;z=4675.;surfaceY=21.5;surfaceDx=0.;surfaceDz=0.;offset=0.;wetness=.5;world='noe\noe.wrp';texture='o\pole2.paa';sourceEligible=$true}
    Assert-Soil $soil $true;Check $true;$soil.texture='o\sandDark.paa';Refuses {Assert-Soil $soil $true};Assert-Soil $soil $false;Check $true
    $base=@('RAIN_WATER_INIT','Wgpu terrain puddle weather: time=200.000 rain=1.000 accumulated=0.6000 uploaded=0.0000','WGPU_RAIN_WATER_SSR_POLICY enabled=0 receiverRange=60 rayRange=25 steps=24 refine=4 scope=current-screen-world')
    $proof=Renderer-Proof $base $arms[0];Check ($proof.terrainWeather.Count -eq 1)
    Refuses {Renderer-Proof $base $arms[1]};Refuses {Renderer-Proof $base $arms[2]}
    $on=@($base[0],$base[1].Replace('uploaded=0.0000','uploaded=0.6000'),$base[2],'Rain water grid: generation=1 revision=2 width=10 height=10 copied=100 enabled=1 accepted=true maxDepth=0.100000 independent=1')
    Check ((Renderer-Proof $on $arms[3]).positiveGridUploads.Count -eq 1)
    Refuses {Renderer-Proof $on $arms[1]};Refuses {Renderer-Proof @($on|ForEach-Object{$_ -replace 'accepted=true','accepted=false'}) $arms[3]}
    $ssr=@($on|ForEach-Object{$_ -replace 'POLICY enabled=0','POLICY enabled=1'})
    Refuses {Renderer-Proof $ssr $arms[4]};$ssr+='WGPU_RAIN_WATER_SSR enabled=1 interest=1 ready=1 size=1280x720 source=pre-rain-world selfFeedback=false'
    Check ((Renderer-Proof $ssr $arms[4]).ssrReady.Count -eq 1)
    Refuses {Number 'NaN'};Refuses {Number $true};Refuses {Pose @(1,2,3)}
    $old=[Threading.Thread]::CurrentThread.CurrentCulture
    try{[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE');Check ((Pose @(1.25,2.5,3.75,0,-86)) -ceq '1.25 2.5 3.75 0 -86')}finally{[Threading.Thread]::CurrentThread.CurrentCulture=$old}
    Check ((Decode-Eval '"OK"') -ceq 'OK')
    $key='POSEIDON_WETGROUND_SELFTEST';$prior=[Environment]::GetEnvironmentVariable($key,'Process')
    try{[Environment]::SetEnvironmentVariable($key,'test','Process');Restore-Environment $key $null;Check ($null -eq [Environment]::GetEnvironmentVariable($key,'Process'))}finally{Restore-Environment $key $prior}
    Write-Host "Wet-ground runner helper checks passed: $checks. No game/GPU/profile used."
}
if($SelfTest){Test-Helpers;return}
$World=$World.ToLowerInvariant()
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit for the installed pair.'
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game has another owner.'
Require ($SingleArm -eq 'Custom' -or !($PSBoundParameters.ContainsKey('Film') -or $PSBoundParameters.ContainsKey('Water') -or $PSBoundParameters.ContainsKey('SSR'))) 'Explicit Film/Water/SSR parameters require -SingleArm Custom.'
foreach($value in @($SearchX,$SearchZ,$SoilX,$SoilZ)){$null=Number $value}
$explicitTarget=![double]::IsNaN($TargetX)
Require ($explicitTarget -eq (![double]::IsNaN($TargetZ))) 'Supply both TargetX and TargetZ.'
if($explicitTarget){$null=Number $TargetX;$null=Number $TargetZ}
function Installed-State{
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    Require ($stamp -match '^codex/streaming-residency\s+([a-fA-F0-9]{8,40})\s+') 'Installed branch/commit stamp missing.'
    $commit=$Matches[1];$common=[Math]::Min($commit.Length,$ExpectedCommit.Length)
    Require ($common -ge 8 -and $commit.Substring(0,$common).Equals($ExpectedCommit.Substring(0,$common),[StringComparison]::OrdinalIgnoreCase)) 'Installed commit differs from requested pair.'
    return [ordered]@{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
function Assert-Pair($a,$b){Require (($a|ConvertTo-Json -Depth 6 -Compress) -ceq ($b|ConvertTo-Json -Depth 6 -Compress)) 'Installed EXE/DLL pair changed.'}
$before=Installed-State
$output=Join-Path $root ('build/wet-ground-ablation/'+$Label+'-'+$World+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $output -Force|Out-Null
$plan=@(Arm-Plan $SingleArm $Film $Water $SSR)
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;installed=$before;expectedCommit=$ExpectedCommit;world=$World;worldLabel=@{abel='Abel/Malden';cain='Cain/Kolgujev';eden='Eden/Everon';noe='Noe/Nogova'}[$World];lockOwner=$env:LOCK_OWNER;plan=$plan;arms=@{};target=$null;
    scope='Same installed pair, fixed real-weather simulation durations, independent processes/private profiles. SoilFilm includes moist-soil shading and cosmetic film. Actual weather/terrain/field changes between launches preclude pixel-perfect isolation. Appearance needs inspection; no per-fragment SSR hit proof or all-map mud admission.'}
$result.helperSha256=@{};foreach($helper in @('Write-BenchmarkGraphics.ps1','Assert-CaptureLifecycle.ps1')){$result.helperSha256[$helper]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $helper)).Hash}
$settings=@{WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_JITTER='0';
    WGR_TERRAIN_PUDDLE_FIXTURE='1';WGR_RAIN_WATER_TRACE='1';WGR_RAIN_WATER_SSR_TRACE='1';POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';WGR_INTERIOR_SKY='1';WGR_INTERIOR_SKY_DEBUG='0';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_TRACE='1'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','POSEIDON_PLAYER_GROUND_TRACE','POSEIDON_UNIFORM_WET_TRACE','POSEIDON_SAND_TRACE','POSEIDON_MUD_TRACE','WGR_UNIFORM_WET')
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR','WGR_TERRAIN_PUDDLES','WGR_RAIN_WATER','WGR_RAIN_WATER_SSR')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result.environment=@{common=$settings;cleared=$clear;inherited=$saved}
$p=$null;$client=$null;$reader=$null;$writer=$null;$arm=$null;$armDir=$null;$log=$null;$deadline=$null
function Live-Lines([string]$path){
    if(!(Test-Path -LiteralPath $path)){return @()}
    $f=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$r=[IO.StreamReader]::new($f)
    try{$text=$r.ReadToEnd();$end=$text.LastIndexOf("`n");if($end -ge 0){return $text.Substring(0,$end).Split("`n")};return @()}finally{$r.Dispose()}
}
function Logs{return @(@($log,$stdout,$stderr)|ForEach-Object{Live-Lines $_})}
function Health{Require (!$p.HasExited) 'Owned game exited early.';Require ([DateTime]::UtcNow -lt $deadline) 'Owned arm exceeded bounded600s.';Require (!(Logs|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot|Cannot load --test-world')) 'Runtime failure in capture logs.'}
function Send($request){
    Health;$json=$request|ConvertTo-Json -Compress;$json|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$writer.WriteLine($json)
    $until=[DateTime]::UtcNow.AddSeconds(30)
    do{Require ([DateTime]::UtcNow -lt $until) 'Harness response exceeded30s.';$line=$reader.ReadLine();Require ($null -ne $line) 'Harness closed.';$line|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
    Require $reply.ok $line;return $reply
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Field{$s=Send @{cmd='dev_rain_water';action='state'};Assert-Field $s;return $s}
function Local([bool]$needWater=$false){$s=Send @{cmd='dev_rain_water';action='sample';x=$result.target.x;z=$result.target.z};Assert-Local $s $needWater;return $s}
function Weather{$s=Send @{cmd='weather_visibility'};foreach($key in @('rain','particleDensity','liquidRain','fog')){$null=Number $s.$key};Require ($s.particleSnowflakes -is [bool] -and !$s.particleSnowflakes -and $s.particleDensity -eq 0) 'Particle override contaminated actual rain.';return $s}
function Advance([double]$seconds){
    $start=Number (Eval 'time');Exec 'setAccTime 8';$until=[DateTime]::UtcNow.AddSeconds($seconds/8+25)
    do{Start-Sleep -Milliseconds 250;$time=Number (Eval 'time');Require ([DateTime]::UtcNow -lt $until) 'Actual simulation failed to advance.'}while($time-$start -lt $seconds)
    Exec 'setAccTime 0';return @{start=$start;end=(Number (Eval 'time'));requestedSeconds=$seconds;accTime=8}
}
function Capture([string]$name,$pose){
    $pre=Field;$localBefore=Local;$weather=Weather;$time=Number (Eval 'time')
    Require ((Eval ('triFreeFlyPose "'+(Pose $pose)+'"')) -ceq 'OK') 'Camera refused.';Start-Sleep -Milliseconds 700
    $path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path -LiteralPath $path)){Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    $post=Field;$localAfter=Local;Assert-Paused $pre $post
    Require ((Number (Eval 'time')) -eq $time) 'Capture advanced paused simulation time.'
    foreach($key in @('valid','depth','height','terrainY','flowX','flowZ','revision')){Require ($localBefore.$key -eq $localAfter.$key) "Camera changed local water: $key"}
    $arm.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=(Get-Item -LiteralPath $path).Length;camera=$pose;time=$time;actualDayTime=(Eval 'dayTime');requestedDate=@(1985,6,21,16,0);weather=$weather;fieldBefore=$pre;fieldAfter=$post;localBefore=$localBefore;localAfter=$localAfter;localWaterPresent=($localBefore.valid -and $localBefore.depth -gt .001)}
}
function Capture-Stage([string]$stage){
    $soil=Send @{cmd='dev_mud';action='sample';x=$SoilX;z=$SoilZ};Assert-Soil $soil $auditedSoil
    $local=Local;$bed=Number $local.terrainY;$surface=$(if($local.valid -and $local.depth -gt .001){Number $local.height}else{$bed})
    $x=$result.target.x;$z=$result.target.z
    $arm.stages[$stage]=@{field=(Field);local=$local;soil=$soil;weather=(Weather);time=(Eval 'time');soilAdmission=$(if($auditedSoil){'audited-Noe-pole2'}else{'unverified-map-ground; naked soil+film control only'})}
    Capture "$stage-soil-close" @($SoilX,($SoilZ-2),($soil.surfaceY+2.2),0,-55)
    Capture "$stage-soil-close-repeat" @($SoilX,($SoilZ-2),($soil.surfaceY+2.2),0,-55)
    Capture "$stage-soil-top" @($SoilX,$SoilZ,($soil.surfaceY+8),0,-86)
    Capture "$stage-soil-grazing" @($SoilX,($SoilZ-8),($soil.surfaceY+1.2),0,-8.5)
    Capture "$stage-soil-close-return" @($SoilX,($SoilZ-2),($soil.surfaceY+2.2),0,-55)
    $waterPose=@($x,($z-24),($surface+9),0,-20.55)
    Capture "$stage-water-oblique" $waterPose
    Capture "$stage-water-oblique-repeat" $waterPose
    Capture "$stage-water-top" @($x,$z,($surface+8),0,-86)
    Capture "$stage-water-return" $waterPose
}
function Close-Owned{
    $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game did not quit normally.';Require ($p.ExitCode -eq 0) 'Owned game exit was not zero.'
    $combined=Join-Path $armDir 'combined.log';Logs|Set-Content -LiteralPath $combined
    $life=Join-Path $armDir 'lifecycle.json';@{status='ok';timed_out=$false;contended=$false;exit_code=$p.ExitCode}|ConvertTo-Json|Set-Content -LiteralPath $life
    & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $life -LogPath $combined
    $arm.exitCode=$p.ExitCode;$arm.lifecycle=$life;$arm.renderer=Renderer-Proof (Logs) $arm.flags
}
$auditedSoil=($World -ceq 'noe' -and $SoilX -eq 4975 -and $SoilZ -eq 4675)
try{
    foreach($flag in $plan){
        Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Another process owns the game.';Assert-Pair $before (Installed-State)
        $armDir=Join-Path $output $flag.name;New-Item -ItemType Directory -Path $armDir -Force|Out-Null
        $profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Path $profile|Out-Null
        & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -MsaaSamples 4 -DlssMode 0
        $graphics=Join-Path $profile 'graphics.cfg';$text=[IO.File]::ReadAllText($graphics).Replace('fpsCap=0;','fpsCap=60;').Replace('brightness=1.6;','brightness=1;');[IO.File]::WriteAllText($graphics,$text)
        foreach($key in $clear){Restore-Environment $key $null};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
        $env:POSEIDON_USER_DIR=$profile;$env:WGR_TERRAIN_PUDDLES=[string]$flag.film;$env:WGR_RAIN_WATER=[string]$flag.water;$env:WGR_RAIN_WATER_SSR=[string]$flag.ssr
        $arm=[ordered]@{status='running';flags=$flag;profile=$profile;profileSha256=(Get-FileHash -LiteralPath $graphics).Hash;installedBefore=(Installed-State);stages=@{};captures=@{};actualEnvironment=@{}}
        foreach($key in $keys){$arm.actualEnvironment[$key]=[Environment]::GetEnvironmentVariable($key,'Process')};$result.arms[$flag.name]=$arm
        $log=Join-Path $armDir 'engine.log';$stdout=Join-Path $armDir 'stdout.txt';$stderr=Join-Path $armDir 'stderr.txt';$deadline=[DateTime]::UtcNow.AddSeconds(600)
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
        $mission=Join-Path $armDir ('field.'+$World);New-Item -ItemType Directory -Path $mission|Out-Null
        Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_sand.noe/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
        $arm.missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash
        $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd',"$ViewDistance",'--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'));$arm.arguments=$args
        $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr;$null=$p.Handle;$arm.pid=$p.Id
        $until=[DateTime]::UtcNow.AddSeconds(120)
        do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}}while(!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission not ready.'
        Exec 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]'
        Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Simulation clock setup refused.';Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Brightness refused.'
        $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
        $initial=Field;Require ($initial.volume -eq 0 -and $initial.rainVolume -eq 0) 'Fresh mission has stale water.';$arm.initial=$initial
        Exec '0 setOvercast 1; 0 setRain 1';$arm.rainAdvance=Advance $RainSimSeconds
        $wet=Field;$weather=Weather;Require ($wet.width -ge 2 -and $wet.volume -gt 0 -and $wet.rainVolume -gt 0 -and $weather.liquidRain -gt 0) 'Actual rain did not create water.'
        if(!$result.target){
            if($explicitTarget){$result.target=@{x=$TargetX;z=$TargetZ;selection='explicit coordinate; actual supported positive wet sample required'}}
            else{
                $spacing=Number $wet.spacing;Require ($spacing -gt 0) 'Missing actual field spacing.';$cx=[Math]::Round($SearchX/$spacing)*$spacing;$cz=[Math]::Round($SearchZ/$spacing)*$spacing;$samples=@()
                foreach($dz in -5..5){foreach($dx in -5..5){$x=$cx+$dx*$spacing;$z=$cz+$dz*$spacing;$sample=Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z};Assert-Local $sample $false;$samples+=@{x=$x;z=$z;sample=$sample}}}
                $arm.searchSamples=$samples;$candidates=@($samples|Where-Object{$_.sample.valid -and $_.sample.depth -gt .001}|Sort-Object {$_.sample.depth} -Descending)
                Require ($candidates.Count -gt 0) 'No supported positive pool near search center.';$result.target=@{x=$candidates[0].x;z=$candidates[0].z;selection='deepest supported first-arm real-rain sample; held fixed across later arms'}
            }
        }
        $null=Local $true;Capture-Stage 'wet'
        Exec '0 setOvercast 0.6; 0 setRain 0; setAccTime 1';Start-Sleep -Seconds 1;Exec 'setAccTime 0'
        $stop=Field;$stopWeather=Weather;Require ($stopWeather.liquidRain -eq 0) 'Storm has not actually stopped raining.';$arm.rainStopped=@{field=$stop;weather=$stopWeather;local=(Local)}
        $arm.drainAdvance=Advance $DrainSimSeconds;$drain=Field;Require ($drain.rainVolume -eq $stop.rainVolume -and $drain.volume -lt $stop.volume) 'Actual no-rain field did not drain.'
        Require ((Weather).liquidRain -eq 0) 'Rain restarted during drainage.';Capture-Stage 'after-rain'
        Exec '0 setOvercast 0';$arm.sunAdvance=Advance $DrainSimSeconds;$sun=Field
        Require ($sun.rainVolume -eq $drain.rainVolume -and $sun.evaporationVolume -gt $drain.evaporationVolume -and $sun.volume -lt $drain.volume) 'Clear-day retained field did not evaporate.'
        Require ((Weather).liquidRain -eq 0) 'Rain restarted during sun drying.';Capture-Stage 'sun'
        Close-Owned;$client.Dispose();$client=$null;$arm.installedAfter=Installed-State;Assert-Pair $before $arm.installedAfter;$arm.status='lifecycle-and-source-gates-passed-appearance-review-pending';$p=$null
    }
    $result.status='all-selected-arms-passed-appearance-review-pending'
}catch{$result.status='failed';$result.error=$_.Exception.Message;if($arm){$arm.status='failed';$arm.error=$_.Exception.Message};throw}
finally{
    if($p -and !$p.HasExited){try{$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Cleanup quit timed out.'}catch{if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if($client){$client.Dispose()}
    foreach($key in $keys){Restore-Environment $key $saved[$key]}
    try{$result.installedAfter=Installed-State;Assert-Pair $before $result.installedAfter}catch{$result.status='failed';$result.pairError=$_.Exception.Message}
    $result|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Wet-ground ablation evidence: $output"
    if($result.status -eq 'failed' -and !$result.error){throw $result.pairError}
}
