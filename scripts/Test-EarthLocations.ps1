<# Installed-game acceptance for live Earth geographic relocation. Run through
   scripts/with-game-lock.sh. Only the process started here is stopped. Uses
   existing cached overview imagery; never downloads a new overview image. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExpectedCommit,
    [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
    [ValidateRange(60,240)][int]$PatchTimeoutSeconds=150,
    [switch]$CachedBuildings,
    [switch]$CachedFeatures,
    [switch]$LocalSurface
)
$ErrorActionPreference='Stop'
function Require($ok,[string]$message){if(!$ok){throw $message}}
Require $env:LOCK_OWNER 'Game lock required; run through scripts/with-game-lock.sh'
$repo=Split-Path -Parent $PSScriptRoot
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved'
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
Require ($stamp.Contains($ExpectedCommit)) 'Installed source differs'
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg'
Require (Test-Path -LiteralPath $imagery) 'Cached NASA overview required; launch Earth preview once before acceptance'
$out=Join-Path $repo ('build/earth-locations/'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Force -Path $out
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $CacheDirectory
$mission=Join-Path $CacheDirectory 'fixture/earth-preview.abel'
$settings=@{
    POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='46.6';POSEIDON_EARTH_LONGITUDE='8.1';
    POSEIDON_EARTH_CACHE=$CacheDirectory;POSEIDON_EARTH_IMAGERY=$imagery;
    POSEIDON_EARTH_BUILDINGS=$(if($CachedBuildings){'1'}else{'0'});
    POSEIDON_EARTH_FEATURES=$(if($CachedFeatures){'1'}else{'0'});
    POSEIDON_EARTH_SURFACE=$(if($LocalSurface){'1'}else{'0'});
    WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';
    POSEIDON_USER_DIR=(Join-Path $out 'user')
}
$saved=@{}
foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
$receipt=@{installed=$stamp;localSurface=[bool]$LocalSurface;views=@();invalid=@();superseded=@();passed=$false;source='NASA overview + Terrarium elevation; optional single cached ACT surface; no worldwide playable objects'}
$p=$null;$client=$null;$writer=$null;$reader=$null
$log=Join-Path $out 'engine.log'
$stderr=Join-Path $out 'stderr.log'
$totalDeadline=[DateTime]::UtcNow.AddSeconds(150+5*$PatchTimeoutSeconds)
$culture=[Globalization.CultureInfo]::InvariantCulture
function Send($command){
    Require (!$p.HasExited) 'Game exited'
    Require ([DateTime]::UtcNow-lt$totalDeadline) 'Overall acceptance deadline'
    $writer.WriteLine(($command|ConvertTo-Json -Depth 10 -Compress))
    do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
    Require $reply.ok $line
    return $reply
}
function Eval([string]$code){return ([string](Send @{cmd='eval';code=$code}).result).Trim('"')}
function Location {
    $value=Eval 'triEarthLocation []'
    Require ($value-match'^OK:latitude=([-+0-9.eE]+) longitude=([-+0-9.eE]+) altitude=([-+0-9.eE]+) revision=([0-9]+)$') ('Invalid origin query: '+$value)
    return @{latitude=[double]::Parse($Matches[1],$culture);longitude=[double]::Parse($Matches[2],$culture);altitude=[double]::Parse($Matches[3],$culture);revision=[uint64]$Matches[4];raw=$value}
}
function Camera {return (Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera.position}
function CheckCamera($position,[double]$altitude){
    Require ($position.Count-eq3) 'Missing actual camera position'
    Require ([Math]::Abs($position[0]-6400)-lt.1 -and [Math]::Abs($position[2]-6400)-lt.1 -and [Math]::Abs($position[1]-$altitude)-lt1) ('Camera did not reset to Earth anchor/altitude: '+($position-join','))
}
function PatchRows {
    foreach($row in @(Select-String -Path $log -Pattern 'Earth terrain patch ')){
        if($row.Line-match'geographic origin \[([-+0-9.eE]+),([-+0-9.eE]+)\], revision ([0-9]+)'){
            @{line=$row.Line;lineNumber=$row.LineNumber;latitude=[double]::Parse($Matches[1],$culture);longitude=[double]::Parse($Matches[2],$culture);revision=[uint64]$Matches[3]}
        }
    }
}
function CheckCache {
    $tiles=@(Get-ChildItem -LiteralPath $CacheDirectory -Directory -Filter 'terrarium-z*' | Get-ChildItem -Filter '*.png')
    Require ($tiles.Count-le256) ('Disk tile bound exceeded: '+$tiles.Count)
    Require (!($tiles|Where-Object {$_.Length-gt2MB})) 'Oversized cached elevation tile'
    return $tiles.Count
}
function WaitPatch($location){
    $deadline=[DateTime]::UtcNow.AddSeconds($PatchTimeoutSeconds)
    do{
        Require (!$p.HasExited) 'Game exited while waiting for geographic patch'
        $rows=@(PatchRows)
        $matching=@($rows|Where-Object {$_.revision-eq$location.revision})
        if($matching.Count){
            $patch=$matching[-1]
            Require ([Math]::Abs($patch.latitude-$location.latitude)-lt.0001 -and [Math]::Abs($patch.longitude-$location.longitude)-lt.0001) 'Patch geographic origin differs from query revision'
            return $patch
        }
        Require ([DateTime]::UtcNow-lt$deadline -and [DateTime]::UtcNow-lt$totalDeadline) ('Patch deadline at '+$location.raw+'; elevation service may be unavailable; no acceptance established')
        Start-Sleep -Milliseconds 300
    }while($true)
}
function CheckNoSuperseded($location){
    $rebase=@(Select-String -Path $log -Pattern ('Earth relocated: .*revision '+$location.revision+'$'))
    Require ($rebase.Count-gt0) 'Missing renderer geographic rebase receipt'
    $after=@(PatchRows|Where-Object {$_.lineNumber-gt$rebase[-1].LineNumber})
    Require (!($after|Where-Object {$_.revision-ne$location.revision})) 'Superseded geographic patch published after latest rebase'
}
function WaitCapture([string]$path){
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length-eq0){
        Require ([DateTime]::UtcNow-lt$deadline) ('Screenshot missing: '+$path)
        Start-Sleep -Milliseconds 100
    }
}
function WaitSurface($location,[bool]$local){
    if(!$LocalSurface){return ''}
    $expected=if($local){'ACT ortho_2023 CC0'}else{'NASA fallback'}
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do{
        $rows=@(Select-String -Path $log -Pattern ('Earth local surface: '+$expected+'.*revision '+$location.revision+'(?:,|$)'))
        if($rows.Count){return $rows[-1].Line}
        Require (!$p.HasExited -and [DateTime]::UtcNow-lt$deadline) ('Local surface publication missing: '+$expected+' '+$location.raw)
        Start-Sleep -Milliseconds 200
    }while($true)
}
function Go([string]$name,[double]$latitude,[double]$longitude,[double]$altitude){
    $previous=Location
    $code='triEarthLocation ['+$latitude.ToString('R',$culture)+','+$longitude.ToString('R',$culture)+','+$altitude.ToString('R',$culture)+']'
    $result=Eval $code
    Require ($result.StartsWith('OK:')) ('Go failed: '+$result)
    $location=Location
    $expectedLongitude=if($longitude-eq180){-180}else{$longitude}
    Require ($location.revision-eq$previous.revision+1) 'Go did not increment origin revision once'
    Require ([Math]::Abs($location.latitude-$latitude)-lt.0001 -and [Math]::Abs($location.longitude-$expectedLongitude)-lt.0001 -and [Math]::Abs($location.altitude-$altitude)-lt1) 'Origin query differs from requested Go'
    $patch=WaitPatch $location
    # Luxembourg can remain inside the Alps' large geographic far window.
    # That is valid regional imagery, not a stale local tile. Canyon/dateline
    # still require NASA fallback and check remote/revision safety.
    $surface=WaitSurface $location ($name-in@('luxembourg','alps'))
    Start-Sleep -Milliseconds 500
    $camera=Camera;CheckCamera $camera $altitude
    CheckNoSuperseded $location
    $path=Join-Path $out ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    WaitCapture $path
    $receipt.views+=@{name=$name;location=$location;camera=$camera;patch=$patch;surface=$surface;screenshot=$path;tiles=(CheckCache);performance=(Send @{cmd='frame_performance'})}
    return $location
}
try{
    $null=New-Item -ItemType Directory -Force -Path $env:POSEIDON_USER_DIR
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','20000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$world+'"'),'--test-world-freefly','6400','6400','4500','30','-20','--test-world-hour','12','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $deadline=[DateTime]::UtcNow.AddSeconds(150)
    do{
        Require (!$p.HasExited) 'Startup exited'
        $client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
        if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}
    }while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000
    $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    while((Eval 'triSceneReady')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
    Require (!(Select-String -Path $log -Pattern 'Cannot load --test-world|Critical error' -Quiet)) 'Fixture world failed to load'
    $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;setDate [1985,6,21,12,0]'}
    $receipt.initial=Location
    Require ((Location).raw-ceq$receipt.initial.raw) 'Origin query changed the revision'
    if($CachedBuildings){
        Require (Test-Path (Join-Path $CacheDirectory 'buildings/2480_306.json')) 'Prepare the single Luxembourg cell before this optional acceptance'
        $null=Go 'luxembourg' 49.61 6.13 1000
        $deadline=[DateTime]::UtcNow.AddSeconds(15)
        do{
            $building=@(Select-String -Path $log -Pattern 'Earth cached buildings: cell \[2480,306\], [1-9][0-9]* buildings')
            if($building.Count){break}
            Require ([DateTime]::UtcNow-lt$deadline) 'Cached Luxembourg building mesh was not published'
            Start-Sleep -Milliseconds 200
        }while($true)
        $receipt.buildings=$building[-1].Line
        if($CachedFeatures){
            $deadline=[DateTime]::UtcNow.AddSeconds(15)
            do{
                $features=@(Select-String -Path $log -Pattern 'Earth cached feature kind [0-4]: [1-9][0-9]* features')
                if(@($features|ForEach-Object {if($_.Line-match'kind ([0-4]):'){$Matches[1]}}|Select-Object -Unique).Count-eq5){break}
                Require ([DateTime]::UtcNow-lt$deadline) 'All five cached landscape feature kinds were not published'
                Start-Sleep -Milliseconds 200
            }while($true)
            $receipt.features=@($features|ForEach-Object Line)
        }
        $pose=Eval 'triFreeFlyPose "6400 6400 1000 30 -45"'
        Require ($pose.StartsWith('OK')) 'Building overview camera refused'
        Start-Sleep -Seconds 1
        $path=Join-Path $out 'luxembourg-buildings.png';$null=Send @{cmd='screenshot';path=$path};WaitCapture $path
        $receipt.localOverview=@{camera=(Camera);performance=(Send @{cmd='frame_performance'});screenshot=$path}
        # Cross east into an uncached centre cell. The previously cached town
        # remains in the bounded 3x3 neighborhood instead of vanishing.
        $beforeNeighbor=@(PatchRows)[-1]
        Require ($beforeNeighbor.line-match'Earth terrain patch ([0-9]+):') 'Missing pre-move patch generation'
        $beforeNeighborGeneration=[uint64]$Matches[1]
        $pose=Eval 'triFreeFlyPose "7500 6400 1000 270 -35"'
        Require ($pose.StartsWith('OK')) 'Neighbor-cell camera refused'
        $deadline=[DateTime]::UtcNow.AddSeconds(15)
        do{
            $neighbor=@(Select-String -Path $log -Pattern 'Earth cached buildings: cell \[2480,307\], [1-9][0-9]* buildings')
            if($neighbor.Count){break}
            Require ([DateTime]::UtcNow-lt$deadline) 'Cached buildings vanished at the adjacent cell boundary'
            Start-Sleep -Milliseconds 200
        }while($true)
        $receipt.neighborBuildings=$neighbor[-1].Line
        if($LocalSurface){
            # Both rolling workers must publish the same terrain generation.
            $deadline=[DateTime]::UtcNow.AddSeconds(30)
            do{
                $patches=@(PatchRows);$last=$patches[-1]
                Require ($last.line-match'Earth terrain patch ([0-9]+):') 'Missing latest patch generation'
                $generation=$Matches[1]
                $surface=@(Select-String -Path $log -Pattern ('Earth local surface: ACT ortho_2023 CC0.*generation '+$generation+', revision '+(Location).revision))
                if($surface.Count -and [uint64]$generation-gt$beforeNeighborGeneration){$receipt.neighborSurface=$surface[-1].Line;break}
                Require ([DateTime]::UtcNow-lt$deadline) 'Neighbor surface generation did not catch up'
                Start-Sleep -Milliseconds 200
            }while($true)
        }
        $path=Join-Path $out 'luxembourg-neighbor.png';$null=Send @{cmd='screenshot';path=$path};WaitCapture $path
    }
    $null=Go 'alps' 46.6 8.1 4500
    $null=Go 'grand-canyon' 36.1 -112.1 6000
    # Fine near window straddles longitude180; verifies tile wrap and UV seam.
    $null=Go 'dateline' 0 179.995 4500
    foreach($code in @('triEarthLocation [85,0,4500]','triEarthLocation [0,181,4500]','triEarthLocation [0,0,499]')){
        $before=Location;$cameraBefore=Camera
        $result=Eval $code
        Require ($result.StartsWith('FAIL:')) ('Invalid Go accepted: '+$code)
        $after=Location;$cameraAfter=Camera
        Require ($after.raw-ceq$before.raw) 'Invalid Go changed geographic origin/revision'
        for($i=0;$i-lt3;$i++){Require ([Math]::Abs($cameraBefore[$i]-$cameraAfter[$i])-lt.1) 'Invalid Go moved the camera'}
        $receipt.invalid+=@{code=$code;result=$result;location=$after;camera=$cameraAfter}
    }
    # Reuse already visited cells. Requests may start on successive frames, but
    # only the last rebase's patch is allowed after its renderer receipt.
    $rapidBefore=Location
    foreach($code in @('triEarthLocation [36.1,-112.1,6000]','triEarthLocation [0,179.995,4500]','triEarthLocation [46.6,8.1,4500]')){
        $result=Eval $code;Require ($result.StartsWith('OK:')) ('Rapid Go failed: '+$result)
        $receipt.superseded+=(Location)
    }
    $final=Location;$patch=WaitPatch $final
    Require ($final.revision-eq$rapidBefore.revision+3) 'Rapid Gos did not produce exactly three revisions'
    Require ([Math]::Abs($final.latitude-46.6)-lt.0001 -and [Math]::Abs($final.longitude-8.1)-lt.0001) 'Rapid final origin differs'
    Start-Sleep -Milliseconds 700
    CheckNoSuperseded $final
    $camera=Camera;CheckCamera $camera 4500
    $path=Join-Path $out 'return-alps.png';$null=Send @{cmd='screenshot';path=$path}
    WaitCapture $path
    $receipt.views+=@{name='return-alps';location=$final;camera=$camera;patch=$patch;screenshot=$path;tiles=(CheckCache);performance=(Send @{cmd='frame_performance'})}
    Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error' -Quiet)) 'Runtime error'
    Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed build changed'
    $receipt.passed=$true
    Write-Host ('Earth live-location acceptance PASS: '+$out)
}catch{
    $receipt.error=$_.Exception.Message
    throw
}finally{
    if($p-and!$p.HasExited){
        if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}}
        if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}
    }
    if($client){$client.Dispose()}
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
