<#
.SYNOPSIS
Measure the opt-in far physical-weather cascade on the installed stock Eden scene.
.DESCRIPTION
Uses farfield-bench's process harness; one readback per launch. Run with-game-lock.sh.
OFF/ON/ON/OFF blocks use the same installed hashes, fixed camera and private profile.
This is a whole-frame differential, not a dedicated weather-pass GPU timer.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
    [ValidateSet('Eden500','Top','Both')][string]$View='Eden500',
    [ValidateRange(1,6)][int]$Repeats=3,
    [ValidateRange(35,45)][int]$WarmupSeconds=35,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='far-weather-cost',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Out='',
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Finite($value){
    Require ($null -ne $value) 'Missing numeric field.'
    $n=[double]::Parse([string]$value,[Globalization.CultureInfo]::InvariantCulture)
    Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite numeric field.'
    return $n
}
function Restore-Environment([string]$key,$value){
    if($null -eq $value){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}
    else{[Environment]::SetEnvironmentVariable($key,$value,'Process')}
}
function Installed-State{
    $state=@{}
    foreach($name in @('OpenPoseidon.exe','wgpu_renderer.dll','DEPLOYED-FROM.txt')){
        $path=Join-Path $GameDir $name
        Require (Test-Path -LiteralPath $path -PathType Leaf) "Installed file absent: $path"
        $state[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $state.stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt'))
    Require ($state.stamp -match ('(?i)'+[regex]::Escape($ExpectedCommit))) 'Installed stamp does not contain ExpectedCommit.'
    return $state
}
function Assert-Pair($before,$after){
    foreach($key in @('OpenPoseidon.exe','wgpu_renderer.dll','DEPLOYED-FROM.txt','stamp')){
        Require ($null -ne $before[$key] -and $before[$key] -ceq $after[$key]) "Installed pair changed: $key"
    }
}
function Validate-Metrics($metrics,$meta,$previous){
    Require ($meta.status -ceq 'ok' -and $meta.timed_out -is [bool] -and !$meta.timed_out -and
        $meta.contended -is [bool] -and !$meta.contended -and $meta.exit_code -eq 0 -and
        $meta.performance_comparable -is [bool] -and $meta.performance_comparable -and $meta.motion_samples -eq 1) 'Run lifecycle or one-readback performance gate failed.'
    Require ($metrics.build.render_width -eq 1280 -and $metrics.build.render_height -eq 720 -and
        $metrics.build.msaa_samples -eq 4 -and $metrics.build.dlss_active -is [bool] -and !$metrics.build.dlss_active) 'Native 1280x720 MSAA4 route not proved.'
    Require ($metrics.gpu_timestamps_available -is [bool] -and $metrics.gpu_timestamps_available) 'GPU timestamps unavailable.'
    Require ((Finite $metrics.cpu_frame_phases_ms.sampled_frames) -ge 200) 'Profiler ring not settled.'
    $total=@($metrics.gpu_timings_ms|Where-Object{$_.name -ceq 'GPU frame total'})
    Require ($total.Count -eq 1 -and (Finite $total[0].milliseconds) -gt 0) 'No unique measured GPU frame total.'
    Require ($metrics.objects.valid -is [bool] -and $metrics.objects.valid) 'Object counter readback unavailable.'
    $population=@{}
    foreach($name in @('registered_instances','main_instances','main_records','main_tris')){
        $n=Finite $metrics.objects.$name
        Require ($n -gt 0 -and $n -eq [Math]::Floor($n)) "Invalid populated object count: $name"
        $population[$name]=$n
        if($null -ne $previous){Require ($previous[$name] -eq $n) "Scene population differs across arms: $name"}
    }
    return @{gpuFrameMs=(Finite $total[0].milliseconds);population=$population}
}
function Validate-Trace([string]$text,[bool]$far){
    Require ($text -notmatch 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Cannot load --test-world') 'Runtime failure in logs.'
    Require ($text -match 'wgpu object snow: enabled=1 deposit=0\.1800 snowlineHeight=-[0-9.]+ snowlineRange=0\.00 snowlineDepth=0\.0000') 'Actual enabled .18m snow uniform upload absent.'
    Require ($text -match 'weather cover map: frame=\d+ instanceEpoch=\d+ image=\d+ resolution=2048 extent=640 retained=[1-9]\d* direct=\d+ ready=true submitted=true') 'Actual near weather publication absent.'
    if($far){
        Require ($text -match 'weather cover far map: frame=\d+ instanceEpoch=\d+ image=\d+ resolution=2048 extent=1600 retained=[1-9]\d* direct=\d+ ready=true submitted=true') 'Actual far weather publication absent.'
    }else{Require ($text -notmatch 'weather cover far map:') 'Far map unexpectedly active in OFF arm.'}
}
function Median($values){
    $ordered=@($values|Sort-Object);Require ($ordered.Count -gt 0) 'No measurements.'
    $i=[int][Math]::Floor($ordered.Count/2)
    if($ordered.Count%2){return [double]$ordered[$i]}
    return ([double]$ordered[$i-1]+[double]$ordered[$i])/2
}
function Get-StockCameraArguments($pose){
    Require ($pose.Count -eq 5) 'Stock camera requires X Z Y azimuth elevation.'
    # Stock Eden must use the mission's legacy world loader. --test-world
    # substitutes LoadOprwModern, which deliberately refuses original OFP WRP.
    $arguments=@('--vd','2000','--test-world-hour','16','--test-world-freefly')
    foreach($component in $pose){
        $arguments += (Finite $component).ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    }
    return $arguments
}
function Test-Helpers{
    $metrics=[pscustomobject]@{build=@{render_width=1280;render_height=720;msaa_samples=4;dlss_active=$false};gpu_timestamps_available=$true;
        cpu_frame_phases_ms=@{sampled_frames=256};gpu_timings_ms=@(@{name='GPU frame total';milliseconds=12.5});
        objects=[pscustomobject]@{valid=$true;registered_instances=2000;main_instances=500;main_records=600;main_tris=40000}}
    $meta=[pscustomobject]@{status='ok';timed_out=$false;contended=$false;exit_code=0;performance_comparable=$true;motion_samples=1}
    $good=Validate-Metrics $metrics $meta $null
    $checks=0
    function Refuses([scriptblock]$action){$refused=$false;try{& $action|Out-Null}catch{$refused=$true};Require $refused 'Unsafe fixture was accepted.'}
    $meta.contended=$true;Refuses {Validate-Metrics $metrics $meta $null};$meta.contended=$false;$checks++
    $meta.motion_samples=6;Refuses {Validate-Metrics $metrics $meta $null};$meta.motion_samples=1;$checks++
    $metrics.objects.main_tris++;Refuses {Validate-Metrics $metrics $meta $good.population};$metrics.objects.main_tris--;$checks++
    $metrics.gpu_timestamps_available=$false;Refuses {Validate-Metrics $metrics $meta $null};$metrics.gpu_timestamps_available=$true;$checks++
    Refuses {Finite 'NaN'};$checks++
    $near='weather cover map: frame=20 instanceEpoch=5 image=1 resolution=2048 extent=640 retained=2000 direct=1 ready=true submitted=true'
    $snow='wgpu object snow: enabled=1 deposit=0.1800 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000'
    $far='weather cover far map: frame=20 instanceEpoch=5 image=1 resolution=2048 extent=1600 retained=2000 direct=1 ready=true submitted=true'
    Validate-Trace ($snow+"`n"+$near+"`n"+$far) $true
    Refuses {Validate-Trace ($snow+"`n"+$near) $true};$checks++
    Refuses {Validate-Trace ($snow+"`n"+$near+"`n"+$far) $false};$checks++
    Refuses {Validate-Trace ($snow+"`n"+$near+"`n"+$far.Replace('submitted=true','submitted=false')) $true};$checks++
    $key='POSEIDON_FAR_WEATHER_HELPER_TEST';$saved=[Environment]::GetEnvironmentVariable($key,'Process')
    try{Restore-Environment $key $null;Require ($null -eq [Environment]::GetEnvironmentVariable($key,'Process')) 'Null restoration retained an empty variable.';$checks++}
    finally{Restore-Environment $key $saved}
    Require ((Median @(1,3,9)) -eq 3 -and (Median @(1,3,5,9)) -eq 4) 'Median calculation failed.'
    $stock=Get-StockCameraArguments @(5025,3725,144.54546,0,-8.53)
    Require (($stock -join ' ') -ceq '--vd 2000 --test-world-hour 16 --test-world-freefly 5025 3725 144.54546 0 -8.53') 'Stock camera arguments changed or use world substitution.'
    Refuses {Get-StockCameraArguments @(5025,3725,144.54546,0)};$checks++
    Refuses {Get-StockCameraArguments @(5025,3725,'NaN',0,-8.53)};$checks++
    Write-Output "Far weather helper checks passed: $checks rejection/restoration cases plus median and stock-loader camera arguments. No game/profile/GPU used."
}
if($SelfTest){Test-Helpers;return}
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit for the installed binary pair.'
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Another game process is running.'
$before=Installed-State
if(!$Out){$Out=Join-Path $root 'build/weather-cover-performance'}
if(![IO.Path]::IsPathRooted($Out)){$Out=Join-Path $root $Out}
$campaign=Join-Path ([IO.Path]::GetFullPath($Out)) ($Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $campaign|Out-Null
$views=[ordered]@{Eden500=@(5025,3725,144.54546,0,-8.53);Top=@(5025,4220,127.5,0,-86)}
$selected=if($View -eq 'Both'){@('Eden500','Top')}else{@($View)}
$settings=@{POSEIDON_TEST_RAIN='0';POSEIDON_SNOW_TEST_DEPTH='0.18';POSEIDON_SNOWLINE='off';POSEIDON_WIND_OVERRIDE='0 90 0';POSEIDON_VSYNC='0';
    WGR_HDR='1';WGR_INTERIOR_SKY='1';WGR_INTERIOR_SKY_DEBUG='0';WGR_GPU_DRIVEN='1';WGR_OBJECT_SNOW='1';WGR_TREE_SNOW='1';WGR_FOREST_SNOW_ATLAS='1';WGR_FOREST_SNOW_COVER_DEBUG='0';WGR_SNOW_POWDER='1';
    WGR_SNOW_SURFACE_FIXTURE='1';WGR_TREE_SNOW_TRACE='0';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_TRACE='1';WGR_GRASS='0';
    WGR_CLOUD_COVERAGE='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_TERRAIN_PUDDLES='0';WGR_GROUND_PUDDLES='0';WGR_RAIN_WATER='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES',
    'POSEIDON_SNOW_SHELTER','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLE_WETNESS',
    'WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_AUTO_EXPOSURE_KEY','WGR_AUTO_EXPOSURE_MIN','WGR_AUTO_EXPOSURE_MAX','WGR_AUTO_EXPOSURE_RATE','WGR_AUTO_EXPOSURE_TAU',
    'WGR_AUTO_EXPOSURE_SKYWEIGHT','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_TONEMAP_WHITE','WGR_TONEMAP_DESAT')
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR','WGR_WEATHER_COVER_FAR');$saved=@{}
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
    installed=$before;expectedCommit=$ExpectedCommit;lockOwner=$env:LOCK_OWNER;environment=$settings;cleared=$clear;inherited=$saved;
    warmupSeconds=$WarmupSeconds;repeatsPerBlock=$Repeats;sequence=@(0,1,1,0);views=$views;blocks=@();summary=@{};
    extraDepthTextureBytes=16777216;
    scope='Whole-frame same-binary differential. Startup snow upload and physical publication traces; no capture-frame map validity, selected owner/LOD, fine snow-mesh refinement, geometry-only cost, total VRAM, or visual acceptance claim.'}
foreach($helper in @('farfield-bench.ps1','Write-BenchmarkGraphics.ps1','Assert-CaptureLifecycle.ps1')){$result[$helper]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $helper)).Hash}
try{
    foreach($key in $clear){Restore-Environment $key $null}
    foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    foreach($viewName in $selected){
        $population=$null;$off=@();$on=@();$sequence=@(0,1,1,0)
        for($blockIndex=0;$blockIndex -lt $sequence.Count;$blockIndex++){
            Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.'
            Assert-Pair $before (Installed-State)
            $flag=[string]$sequence[$blockIndex];$blockLabel=$viewName+'-block'+($blockIndex+1)+'-far'+$flag
            $blockDir=Join-Path $campaign $blockLabel;$user=Join-Path $blockDir 'user';New-Item -ItemType Directory -Path $user -Force|Out-Null
            & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0 -MsaaSamples 4
            $profile=Join-Path $user 'graphics.cfg';[IO.File]::WriteAllText($profile,([IO.File]::ReadAllText($profile).Replace('brightness=1.6;','brightness=1;')))
            $block=@{view=$viewName;far=$flag;label=$blockLabel;profile=$profile;profileSha256=(Get-FileHash -LiteralPath $profile).Hash;status='running';runs=@()}
            $result.blocks+=,$block
            $env:POSEIDON_USER_DIR=$user;$env:WGR_WEATHER_COVER_FAR=$flag
            $stockArguments=Get-StockCameraArguments $views[$viewName]
            & "$PSScriptRoot/farfield-bench.ps1" -Label $blockLabel -Mission 'tests/perf/missions/perf_field.eden' -Out $campaign -Repeats $Repeats -GameDir $GameDir -Backend wgpu -Width 1280 -Height 720 -Windowed -WarmupSeconds $WarmupSeconds -TimeoutSeconds ($WarmupSeconds+180) -MotionSamples 1 -RequireAll -ExtraArgs $stockArguments
            Require ($LASTEXITCODE -eq 0) 'farfield-bench rejected a run; inspect retained lifecycle metadata.'
            for($repeat=1;$repeat -le $Repeats;$repeat++){
                $stem=Join-Path $blockDir ('run-{0:D2}' -f $repeat)
                & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath "$stem.meta.json" -LogPath "$stem.log"
                $metrics=Get-Content -LiteralPath "$stem.json" -Raw|ConvertFrom-Json;$meta=Get-Content -LiteralPath "$stem.meta.json" -Raw|ConvertFrom-Json
                $verified=Validate-Metrics $metrics $meta $population
                if($null -eq $population){$population=$verified.population}
                $text=@("$stem.log","$stem.stdout.txt","$stem.stderr.txt"|ForEach-Object{[IO.File]::ReadAllText($_)}) -join "`n"
                Validate-Trace $text ($flag -ceq '1')
                $block.runs+=@{metrics="$stem.json";metadata="$stem.meta.json";png="$stem.png";pngSha256=(Get-FileHash -LiteralPath "$stem.png").Hash;gpuFrameMs=$verified.gpuFrameMs;objects=$metrics.objects;cpu=$metrics.cpu_frame_phases_ms}
                if($flag -ceq '1'){$on+=,$verified.gpuFrameMs}else{$off+=,$verified.gpuFrameMs}
            }
            Assert-Pair $before (Installed-State);$block.status='measured'
        }
        $offMedian=Median $off;$onMedian=Median $on
        $result.summary[$viewName]=@{offGpuFrameMs=$off;onGpuFrameMs=$on;offMedianMs=$offMedian;onMedianMs=$onMedian;
            deltaMedianMs=($onMedian-$offMedian);deltaPercent=(100*($onMedian-$offMedian)/$offMedian);population=$population;
            interpretation='Descriptive differential; no invented budget or dedicated far-pass attribution.'}
    }
    $result.status='measured-not-budget-or-visually-accepted'
}catch{$result.status='failed';$result.error=$_.Exception.Message;throw}
finally{
    foreach($key in $keys){Restore-Environment $key $saved[$key]}
    $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $campaign 'result.json') -Encoding UTF8
    Write-Output "Far weather performance record: $campaign"
}
