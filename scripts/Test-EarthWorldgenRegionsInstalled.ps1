<# Parent/operator runs through scripts/with-game-lock.sh after matched deployment.
   Three captures: city -> Mamer -> city, plus one bounded actual SDL D movement.
   -SelfTest reads no game state. No downloads, deployment or source generation.
   A diagnostic PASS is not visual, collision or upstream playability acceptance. #>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit,
    [string]$Index='D:/OpenPoseidon-Worldgen-Lab-20261008/worldgen-regions-complete.json',
    [string]$AddonRoot='D:/OpenPoseidon-Worldgen-Lab-20261008/mamer-worldgen-complete-02/generated/@CWR-Milestone4',
    [ValidateRange(1,255)][int]$CityRegionId=1,
    [ValidateRange(1,255)][int]$MamerRegionId=2,
    [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [ValidateRange(30,180)][int]$PoseTimeoutSeconds=120,
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Finite([double]$value){return (![double]::IsNaN($value)-and![double]::IsInfinity($value))}
function Fmt([double]$value){Require (Finite $value) 'Nonfinite coordinate';return $value.ToString('R',$culture)}
function ReceiptJson($receipt){return ($receipt|ConvertTo-Json -Depth 30)}
function SaveReceipt($receipt,[string]$path){
    # Convert first so a serialization error cannot truncate the last checkpoint.
    $json=ReceiptJson $receipt;$temporary=$path+'.partial'
    $json|Set-Content -LiteralPath $temporary
    Move-Item -LiteralPath $temporary -Destination $path -Force
}
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require ($value.EndsWith('"')) 'Malformed SQF string';return $value.Substring(1,$value.Length-2).Replace('""','"')};return $value}
function ParseLocation([string]$value){
    Require ($value-match'^OK:latitude=([-+0-9.eE]+) longitude=([-+0-9.eE]+) altitude=([-+0-9.eE]+) revision=(\d+)$') ('Malformed Earth location: '+$value)
    return @{latitude=[double]::Parse($Matches[1],$culture);longitude=[double]::Parse($Matches[2],$culture);altitude=[double]::Parse($Matches[3],$culture);revision=[uint64]$Matches[4];raw=$value}
}
function ViewError($camera,[double]$heading,[double]$pitch){
    Require ($camera.direction.Count-eq3) 'Actual camera direction missing'
    $az=$heading*[Math]::PI/180;$el=$pitch*[Math]::PI/180
    $expected=@(([Math]::Sin($az)*[Math]::Cos($el)),[Math]::Sin($el),([Math]::Cos($az)*[Math]::Cos($el)))
    $squared=0.0;for($j=0;$j-lt3;$j++){$actual=[double]$camera.direction[$j];Require (Finite $actual) 'Nonfinite actual view';$squared+=[Math]::Pow($actual-$expected[$j],2)}
    return [Math]::Sqrt($squared)
}
function ParsePublication([string]$line){
    if($line-match'Earth Worldgen publication: revision (\d+) generation (\d+) source (\d+) covered (\d+) selected (\d+) modelLimitSkipped (\d+) error(.*)$'){
        return @{revision=[uint64]$Matches[1];generation=[uint64]$Matches[2];source=[int]$Matches[3];covered=[int]$Matches[4];selected=[int]$Matches[5];modelLimitSkipped=[int]$Matches[6];error=$Matches[7].Trim();raw=$line}
    }
}
function ParseResidency([string]$line){
    if($line-match'Earth Worldgen resident regions: (\d+) IDs \[(\d+),(\d+)\] sourceBytes (\d+) valueBytes (\d+); offline prepared index only'){
        $count=[int]$Matches[1];$first=[int]$Matches[2];$second=[int]$Matches[3]
        return @{count=$count;ids=@(@($first,$second)|Select-Object -First $count);sourceBytes=[uint64]$Matches[4];valueBytes=[uint64]$Matches[5];raw=$line}
    }
}
function ParseInstalled([string]$line){
    if($line-match'Earth Worldgen installed assets: revision (\d+) generation (\d+) created (\d+) attempted (\d+) models (\d+); presentation only'){
        return @{revision=[uint64]$Matches[1];generation=[uint64]$Matches[2];created=[int]$Matches[3];attempted=[int]$Matches[4];models=[int]$Matches[5];raw=$line}
    }
}
function Families($lines){
    $models=@(foreach($line in $lines){if($line-match'Earth Worldgen installed model: (.+?\.p3d) retained \d+ levels ([1-9]\d*) positions ([1-9]\d*) mapType \d+ rigidAuthored 1'){$Matches[1].ToLowerInvariant()}})
    return @{models=@($models|Sort-Object -Unique);tree=[bool]@($models|Where-Object {$_-match'^data3d[\\/]str[ _].+\.p3d$'}).Count;road=[bool]@($models|Where-Object {$_-match'^o[\\/]road[\\/].+\.p3d$'}).Count;house=[bool]@($models|Where-Object {$_-match'^o[\\/]hous[\\/].+\.p3d$|^data3d[\\/]dum_.+\.p3d$'}).Count}
}
function Selection($lines,[int]$after,[uint64]$revision,[uint64]$generation){
    $block=$null
    for($i=$after;$i-lt$lines.Count;$i++){
        $pub=ParsePublication $lines[$i]
        if($pub){
            $block=$null
            if($pub.revision-eq$revision-and$pub.generation-eq$generation){$block=@{publication=$pub;start=$i;residency=$null;installed=$null}}
        }
        if(!$block){continue}
        $resident=ParseResidency $lines[$i];if($resident){$block.residency=$resident}
        $installed=ParseInstalled $lines[$i]
        if($installed-and$installed.revision-eq$revision-and$installed.generation-eq$generation){$block.installed=$installed;$block.end=$i}
    }
    if($block-and$block.installed-and$block.residency){
        $block.modelLines=@($lines[($block.start+1)..$block.end]);$block.families=Families $block.modelLines
        return $block
    }
}
function ValidateSelection($block,[int]$requiredId,[int]$retiredId=0){
    $pub=$block.publication;$resident=$block.residency;$installed=$block.installed
    Require ([string]::IsNullOrEmpty($pub.error)) ('Worldgen publication error: '+$pub.error)
    Require ($pub.source-gt0-and$pub.source-le262144-and$pub.covered-ge$pub.selected-and$pub.selected-gt0-and$pub.selected-le2048-and$pub.modelLimitSkipped-eq0) 'Invalid bounded source/selection counts'
    Require ($resident.count-ge1-and$resident.count-le2-and$resident.ids.Count-eq$resident.count-and$resident.ids-contains$requiredId) 'Required prepared region is not resident'
    Require (@($resident.ids|Where-Object {$_-lt1-or$_-gt255}).Count-eq0) 'Invalid resident namespace ID'
    Require ($resident.sourceBytes-gt0-and$resident.sourceBytes-le67108864-and$resident.valueBytes-gt0-and$resident.valueBytes-le67108864) 'Aggregate resident source storage exceeds bounds'
    if($retiredId){Require ($resident.ids-notcontains$retiredId) 'Previous distant prepared region remains resident'}
    Require ($installed.attempted-eq$pub.selected-and$installed.created-eq$installed.attempted-and$installed.models-gt0-and$installed.models-le64) 'Not every selected placement registered within global instance/model caps'
    Require (@($block.modelLines|Where-Object {$_-match'Earth Worldgen model unavailable/bounded:|Earth Worldgen model not retained:'}).Count-eq0) 'Unavailable Worldgen selected model'
    Require ($block.families.tree-and$block.families.road-and$block.families.house) 'Successful stock tree, road and house registration required in current selection'
}
if($SelfTest){
    $rows=@('Earth Worldgen publication: revision 7 generation 11 source 105790 covered 7000 selected 2048 modelLimitSkipped 0 error ',
        'Earth Worldgen upstream final validation: failed; asset presentation only',
        'Earth Worldgen resident regions: 2 IDs [1,2] sourceBytes 13901848 valueBytes 12858304; offline prepared index only',
        'Earth Worldgen installed model: data3d\str smrk.p3d retained 0 levels 3 positions 100 mapType 1 rigidAuthored 1',
        'Earth Worldgen installed model: o\road\sil25.p3d retained 2 levels 2 positions 100 mapType 1 rigidAuthored 1',
        'Earth Worldgen installed model: data3d\dum_mesto2.p3d retained 3 levels 4 positions 200 mapType 1 rigidAuthored 1',
        'Earth Worldgen installed assets: revision 7 generation 11 created 2048 attempted 2048 models 3; presentation only')
    $block=Selection $rows 0 7 11;Require ($null-ne$block) 'Current publication parser';ValidateSelection $block 1
    Require ((ParseLocation 'OK:latitude=49.61000000 longitude=6.13000000 altitude=1000.000 revision=7').revision-eq7) 'Location parser'
    Require ((Decode '"{""covered"":false}"')-ceq'{"covered":false}') 'Ground SQF decode'
    Require ($null-eq(Selection $rows 0 8 11)) 'Stale geographic revision accepted'
    Require ($null-eq(Selection $rows 0 7 12)) 'Stale ground generation accepted'
    Require ($null-eq(Selection ($rows+@('Earth Worldgen publication: revision 7 generation 11 source 105790 covered 7000 selected 2048 modelLimitSkipped 0 error ')) 0 7 11)) 'Later incomplete selection accepted as earlier completed selection'
    foreach($kind in @('missing','retired','oversized','partial','failed')){
        $copy=Selection $rows 0 7 11
        switch($kind){missing{$copy.residency.ids=@(2)} retired{} oversized{$copy.publication.selected=2049} partial{$copy.installed.created=2047} failed{$copy.publication.error='region 1: missing'}}
        $rejected=$false;try{if($kind-eq'retired'){ValidateSelection $copy 1 2}else{ValidateSelection $copy 1}}catch{$rejected=$true}
        Require $rejected ('False acceptance: '+$kind)
    }
    Require (!(Families @('Earth Worldgen model unavailable/bounded: data3d\str smrk.p3d')).tree) 'Unavailable model falsely registered'
    Require ((ViewError @{direction=@(0,0,1)} 0 0)-lt1e-9) 'Compass view parser'
    Require ((ViewError @{direction=@(0,0,1)} 90 -35)-gt1) 'Wrong capture direction accepted'
    $sources=@{};$id=2;$sources[([string]$id)]=@{id=$id;manifest='mamer/manifest.json';bbox=@(49.59,6,49.64,6.08)}
    $receipt=ReceiptJson @{passed=$false;sources=$sources;poses=@(@{selection=$block})}|ConvertFrom-Json
    Require (!$receipt.passed-and$receipt.sources.'2'.id-eq2-and$receipt.sources.'2'.manifest-ceq'mamer/manifest.json'-and$receipt.poses[0].selection.installed.created-eq2048) 'Nested receipt JSON string-key roundtrip'
    $checkpoint=[IO.Path]::GetTempFileName()
    try{
        SaveReceipt @{passed=$false;sources=$sources} $checkpoint;$saved=(Get-FileHash -LiteralPath $checkpoint).Hash
        $rejected=$false;try{SaveReceipt @{sources=@{2=@{id=2}}} $checkpoint}catch{$rejected=$true}
        Require ($rejected-and(Get-FileHash -LiteralPath $checkpoint).Hash-ceq$saved) 'Integer dictionary keys did not fail before preserving previous receipt checkpoint'
    }finally{Remove-Item -LiteralPath $checkpoint -ErrorAction SilentlyContinue;Remove-Item -LiteralPath ($checkpoint+'.partial') -ErrorAction SilentlyContinue}
    Write-Host 'Worldgen region installed-test parser, stale-publication, cap, retirement, model and JSON checkpoint controls PASS';return
}
Require (![string]::IsNullOrEmpty($ExpectedCommit)-and![string]::IsNullOrEmpty($env:LOCK_OWNER)) 'ExpectedCommit and scripts/with-game-lock.sh are required'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved'
Require ($CityRegionId-ne$MamerRegionId) 'City and Mamer must use distinct stable index IDs'
$stampPath=Join-Path $GameDir 'DEPLOYED-FROM.txt';$stamp=(Get-Content -LiteralPath $stampPath -Raw).Trim()
Require ($stamp.Contains($ExpectedCommit)) 'Installed source differs from ExpectedCommit'
$Index=(Resolve-Path -LiteralPath $Index).Path;$AddonRoot=(Resolve-Path -LiteralPath $AddonRoot).Path;$CacheDirectory=(Resolve-Path -LiteralPath $CacheDirectory).Path
Require ((Get-Item -LiteralPath $Index).Length-le65536) 'Region index exceeds 64 KiB'
$catalog=Get-Content -LiteralPath $Index -Raw|ConvertFrom-Json
Require ($catalog.earth_worldgen_regions-eq1-and$catalog.regions.Count-eq2) 'This bounded fixture requires exactly city and Mamer index entries'
$indexDirectory=Split-Path -Parent $Index;$identities=@{};$regionSources=@{};$seenPaths=@{}
$identities[$Index]=(Get-FileHash -LiteralPath $Index).Hash
foreach($region in $catalog.regions){
    $id=[int]$region.id;$idKey=[string]$id;Require ($id-ge1-and$id-le255-and[double]$region.id-eq$id-and!$regionSources.ContainsKey($idKey)) 'Invalid/duplicate region ID'
    $relative=[string]$region.manifest
    Require (![string]::IsNullOrEmpty($relative)-and$relative.Length-le512-and![IO.Path]::IsPathRooted($relative)-and!$relative.Contains('..')-and!$relative.Contains(':')) 'Index manifests must be contained relative paths'
    $manifestPath=[IO.Path]::GetFullPath((Join-Path $indexDirectory $relative));$prefix=$indexDirectory.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    Require ($manifestPath.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)-and!$seenPaths.ContainsKey($manifestPath)) 'Escaping/duplicate manifest path';$seenPaths[$manifestPath]=$true
    $source=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    Require ($region.bbox.Count-eq4-and$source.world.bbox.Count-eq4-and$source.world.name-match'^[a-z0-9_]{1,20}$') 'Malformed indexed Worldgen manifest'
    for($j=0;$j-lt4;$j++){Require ((Finite ([double]$region.bbox[$j]))-and[Math]::Abs([double]$region.bbox[$j]-[double]$source.world.bbox[$j])-le1e-9) 'Index/manifest bbox mismatch'}
    Require ([double]$region.bbox[0]-ge-84-and[double]$region.bbox[2]-le84-and[double]$region.bbox[0]-lt[double]$region.bbox[2]-and[double]$region.bbox[1]-ge-180-and[double]$region.bbox[3]-le180-and[double]$region.bbox[1]-lt[double]$region.bbox[3]) 'Non-crossing geographic bounds required'
    $wrp=Join-Path (Split-Path -Parent $manifestPath) ('source/'+$source.world.name+'/'+$source.world.name+'.wrp')
    if(!(Test-Path -LiteralPath $wrp -PathType Leaf)){$wrp=Join-Path (Split-Path -Parent $manifestPath) ('source/'+$source.world.name+'.wrp')}
    Require ((Get-Item -LiteralPath $wrp).Length-le33554432) 'WRP exceeds per-source 32 MiB cap'
    $identities[$manifestPath]=(Get-FileHash -LiteralPath $manifestPath).Hash;$identities[$wrp]=(Get-FileHash -LiteralPath $wrp).Hash
    $regionSources[$idKey]=@{id=$id;manifest=$manifestPath;wrp=$wrp;bbox=$region.bbox;validation=$source.final_validation}
}
Require ($regionSources.ContainsKey(([string]$CityRegionId))-and$regionSources.ContainsKey(([string]$MamerRegionId))) 'Requested fixture IDs absent from index'
foreach($pose in @(@{id=$CityRegionId;lat=49.61;lon=6.13},@{id=$MamerRegionId;lat=49.615;lon=6.04})){
    $box=$regionSources[([string]$pose.id)].bbox;Require ($pose.lat-ge$box[0]-and$pose.lat-le$box[2]-and$pose.lon-ge$box[1]-and$pose.lon-le$box[3]) 'Fixture origin not covered by its indexed region'
}
$addonArchiveRoot=$AddonRoot;if(!@(Get-ChildItem -LiteralPath $addonArchiveRoot -File -Filter '*.pbo').Count){$addonArchiveRoot=Join-Path $AddonRoot 'AddOns'}
Require (@(Get-ChildItem -LiteralPath $addonArchiveRoot -File -Filter '*.pbo' -ErrorAction SilentlyContinue).Count-gt0) 'Mount the Mamer addon archive root for its generated water-tower P3D'
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg';Require (Test-Path -LiteralPath $imagery) 'Cached NASA overview required; no test downloads'
$repo=Split-Path -Parent $PSScriptRoot;$out=Join-Path $repo ('build/earth-worldgen-regions-installed/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
$null=New-Item -ItemType Directory -Force -Path $out
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $CacheDirectory;$mission=Join-Path $CacheDirectory 'fixture/earth-preview.abel'
$user=Join-Path ([IO.Path]::GetTempPath()) ('op-wgr-'+[Guid]::NewGuid().ToString('N').Substring(0,8));$null=New-Item -ItemType Directory -Path $user
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -MsaaSamples 4 -DlssMode 0
Add-Content -LiteralPath (Join-Path $user 'graphics.cfg') -Value 'optionalReShade=0;'
$settings=@{POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='49.61';POSEIDON_EARTH_LONGITUDE='6.13';POSEIDON_EARTH_CACHE=$CacheDirectory;POSEIDON_EARTH_IMAGERY=$imagery;
    POSEIDON_EARTH_FEATURES='0';POSEIDON_EARTH_BUILDINGS='0';POSEIDON_EARTH_SURFACE='1';POSEIDON_EARTH_CONTENT_URL='';POSEIDON_EARTH_WORLDGEN_MANIFEST=$Index;
    POSEIDON_USER_DIR=$user;WGPU_BACKEND='vulkan';WGR_DLSS='0';WGR_RENDER_SCALE='1';WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';
    WGR_RESHADE_SCENE_DEPTH='0';VK_INSTANCE_LAYERS=$null;VK_ADD_LAYER_PATH=$null;VK_LOADER_LAYERS_ENABLE=$null;RESHADE_BASE_PATH_OVERRIDE=$null;
    HTTPS_PROXY='http://127.0.0.1:9';HTTP_PROXY='http://127.0.0.1:9';ALL_PROXY='http://127.0.0.1:9';NO_PROXY=''}
$saved=@{};$exists=@{};$p=$null;$client=$null;$reader=$null;$writer=$null;$heldKey=$null
$log=Join-Path $out 'engine.log';$stderr=Join-Path $out 'stderr.log';$overall=[DateTime]::UtcNow.AddSeconds(240+4*$PoseTimeoutSeconds)
$result=@{installed=$stamp;expectedCommit=$ExpectedCommit;index=$Index;sources=$regionSources;sourceHashes=$identities;addonArchiveRoot=$addonArchiveRoot;cacheDirectory=$CacheDirectory;userDirectory=$user;
    passed=$false;visualAccepted=$false;playabilityAccepted=$false;stablePlacementIdsObserved=$false;stablePlacementIdsReason='Runtime bridge/logs expose resident region IDs and counts, not placement IDs or retired GPU slots. Stable placement namespaces are CPU-tested separately.';
    poses=@();offline=$true;scope='Bounded prepared Worldgen region residency and retained model registration. Images need inspection; no global data completeness, collision, road navigation or full model-family quality claim.'}
$receiptPath=Join-Path $out 'result.json';SaveReceipt $result $receiptPath
function Health{Require ($null-ne$p-and!$p.HasExited) 'Owned game exited';Require ([DateTime]::UtcNow-lt$overall) 'Overall region acceptance deadline'}
function Send($command){Health;$writer.WriteLine(($command|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require (![string]::IsNullOrEmpty($line)) 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require ([bool]$reply.ok) $line;return $reply}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Location{return ParseLocation (Eval 'triEarthLocation []')}
function Ground([double]$x,[double]$z){return (Eval ('triEarthGround ['+(Fmt $x)+','+(Fmt $z)+']')|ConvertFrom-Json)}
function Camera{return (Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera}
function LineCount{return @(Get-Content -LiteralPath $log).Count}
function WaitFrames([int]$count){$start=[double](Eval 'triFrameCount');$until=[DateTime]::UtcNow.AddSeconds(20);do{Require ([DateTime]::UtcNow-lt$until) 'Rendered-frame deadline';Start-Sleep -Milliseconds 50}while([double](Eval 'triFrameCount')-lt$start+$count)}
function WaitGround($location,[double]$x,[double]$z,[uint64]$afterGeneration=0){
    $until=[DateTime]::UtcNow.AddSeconds($PoseTimeoutSeconds)
    do{
        $g=Ground $x $z;Require ($g.revision-eq$location.revision) 'Ground revision superseded during fixture'
        if($g.covered-and$g.generation-gt$afterGeneration){
            $uploads=@(Select-String -LiteralPath $log -Pattern ('Earth terrain patch '+$g.generation+': .*revision '+$g.revision+'$'))
            if($uploads.Count){return @{ground=$g;upload=$uploads[-1].Line}}
        }
        Require ([DateTime]::UtcNow-lt$until) 'Fresh resident ground deadline; this offline cache may lack the selected window';Start-Sleep -Milliseconds 250
    }while($true)
}
function WaitSelection($location,[double]$x,[double]$z,[int]$after,[int]$requiredId,[int]$retiredId=0){
    $until=[DateTime]::UtcNow.AddSeconds($PoseTimeoutSeconds)
    do{
        $g=Ground $x $z;Require ($g.revision-eq$location.revision) 'Selection ground belongs to superseded origin'
        $lines=@(Get-Content -LiteralPath $log)
        if($g.covered){
            for($i=$after;$i-lt$lines.Count;$i++){
                $pub=ParsePublication $lines[$i]
                if($pub-and$pub.revision-eq$g.revision-and$pub.generation-eq$g.generation){Require ([string]::IsNullOrEmpty($pub.error)) ('Worldgen publication error: '+$pub.error)}
            }
            $block=Selection $lines $after $g.revision $g.generation
            if($block){ValidateSelection $block $requiredId $retiredId;$block.ground=$g;return $block}
        }
        Require ([DateTime]::UtcNow-lt$until) 'Current region selection did not finish admission';Start-Sleep -Milliseconds 250
    }while($true)
}
function Pose([double]$x,[double]$z,[double]$y,[double]$heading,[double]$pitch){Require ((Eval ('triFreeFlyPose "'+(Fmt $x)+' '+(Fmt $z)+' '+(Fmt $y)+' '+(Fmt $heading)+' '+(Fmt $pitch)+'"'))-ceq'OK') 'Freefly pose refused'}
try{
    foreach($key in $settings.Keys){$exists[$key]=Test-Path -LiteralPath ('Env:'+$key);$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $argv=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','3000','--harness',"$port",'--addon-root',$addonArchiveRoot,'--test-mission',$mission,'--test-world',$world,'--test-world-freefly','6400','6400','1000','0','-20','--test-world-hour','12','--log-file',$log)
    $result.arguments=$argv;$result.environment=$settings
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList (($argv|ForEach-Object {'"'+$_+'"'})-join' ') -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $result.ownedPID=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(150)
    do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$until) 'Harness startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=15000;$stream.WriteTimeout=15000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    while((Eval 'triSceneReady')-cne'OK'){Require ([DateTime]::UtcNow-lt$until) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
    $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;setDate [1985,6,21,12,0];setAccTime 1;triDevPanelShow 0;triCursorMove [0,0]'}
    $poses=@(@{name='city-first';lat=49.61;lon=6.13;id=$CityRegionId;retire=0;x=6625.248;z=6857.043;heading=5.56},
        @{name='mamer';lat=49.615;lon=6.04;id=$MamerRegionId;retire=$CityRegionId;x=6107.356;z=6881.220;heading=281.146},
        @{name='city-return';lat=49.61;lon=6.13;id=$CityRegionId;retire=0;x=6625.248;z=6857.043;heading=5.56})
    foreach($pose in $poses){
        $arm=@{name=$pose.name;requestedOrigin=@($pose.lat,$pose.lon);expectedRegion=$pose.id;retiredRegion=$pose.retire;x=$pose.x;z=$pose.z;heading=$pose.heading;pitch=-35;visualAccepted=$false};$result.poses+=,$arm
        $before=Location;$oldGround=Ground 6400 6400;$marker=LineCount
        $location=ParseLocation (Eval ('triEarthLocation ['+(Fmt $pose.lat)+','+(Fmt $pose.lon)+',1000]'))
        Require ($location.revision-eq$before.revision+1-and[Math]::Abs($location.latitude-$pose.lat)-lt.0001-and[Math]::Abs($location.longitude-$pose.lon)-lt.0001) 'Geographic Go did not establish exact new origin revision'
        $arm.location=$location;$arm.oldGround=$oldGround;$arm.immediateGround=Ground 6400 6400
        Require ($arm.immediateGround.revision-eq$location.revision-and($arm.immediateGround.generation-eq0-or$arm.immediateGround.generation-gt$oldGround.generation)) 'Old ground generation survived geographic Go'
        $arm.freshSupport=WaitGround $location 6400 6400 $oldGround.generation
        if($pose.name-eq'city-first'){
            $ordinaryPoseMarker=LineCount;Pose 6655 $pose.z 1000 0 -20;WaitFrames 3
            $null=WaitSelection $location 6655 $pose.z $ordinaryPoseMarker $CityRegionId
            $start=Camera;Require ([Math]::Abs([double]$start.position[0]-6655)-lt.1) 'Ordinary movement arm starts outside intended request-cell edge'
            $moveMarker=LineCount;$frame=[double](Eval 'triFrameCount');$until=[DateTime]::UtcNow.AddSeconds(6)
            try{
                $heldKey=7;$null=Send @{cmd='key';sc=7;hold=$true}
                do{Start-Sleep -Milliseconds 40;$during=Camera;Require ([DateTime]::UtcNow-lt$until) 'Actual SDL D did not move across a 256m request-cell boundary'}while([double]$during.position[0]-[double]$start.position[0]-lt4)
            }finally{$null=Send @{cmd='key_up';sc=7};$heldKey=$null}
            WaitFrames 3;$after=Camera;$dx=[double]$after.position[0]-[double]$start.position[0]
            Require ($dx-ge4-and$dx-lt250-and(Location).revision-eq$location.revision-and[double](Eval 'triFrameCount')-gt$frame) 'Ordinary camera movement missing, excessive or geographically rebased'
            Require ([Math]::Floor([double]$start.position[0]/256)-lt[Math]::Floor([double]$after.position[0]/256)) 'Ordinary movement did not cross selection request-cell boundary'
            $move=WaitSelection $location $after.position[0] $after.position[2] $moveMarker $CityRegionId
            $result.ordinaryMovement=@{before=$start;after=$after;eastMovementMetres=$dx;selection=$move;input='Actual held SDL D/scancode7, released before each Go'}
            $null=Send @{cmd='exec';code='setAccTime 0'}
        }
        $support=WaitGround $location $pose.x $pose.z;$arm.y=[double]$support.ground.height+15;$poseMarker=LineCount
        Pose $pose.x $pose.z $arm.y $pose.heading -35;WaitFrames 3
        $settleUntil=[DateTime]::UtcNow.AddSeconds($PoseTimeoutSeconds)
        do{
            $selection=WaitSelection $location $pose.x $pose.z $poseMarker $pose.id $pose.retire
            WaitFrames 30;$arm.groundAfter=Ground $pose.x $pose.z
            if($arm.groundAfter.covered-and$arm.groundAfter.generation-eq$selection.publication.generation){break}
            Require ([DateTime]::UtcNow-lt$settleUntil) 'Ground/selection did not remain settled across 30 rendered frames'
        }while($true)
        $arm.camera=Camera
        Require ($arm.groundAfter.covered-and$arm.groundAfter.revision-eq$location.revision-and$arm.groundAfter.generation-eq$selection.publication.generation-and(Location).revision-eq$location.revision) 'Ground changed before settled capture'
        Require ([Math]::Abs([double]$arm.camera.position[0]-$pose.x)-lt.1-and[Math]::Abs([double]$arm.camera.position[2]-$pose.z)-lt.1-and[Math]::Abs([double]$arm.camera.position[1]-$arm.y)-lt.1) 'Actual camera differs from ground-relative capture pose'
        $arm.viewError=ViewError $arm.camera $pose.heading -35;Require ($arm.viewError-lt.0001) 'Actual camera direction differs from requested capture view'
        $arm.clearance=[double]$arm.camera.position[1]-[double]$arm.groundAfter.height;Require ($arm.clearance-gt5-and$arm.clearance-lt30) 'Capture is not close above actual current resident ground'
        $arm.selection=$selection;$arm.performance=Send @{cmd='frame_performance'}
        Require ($arm.performance.renderWidth-eq1600-and$arm.performance.renderHeight-eq900-and$arm.performance.outputWidth-eq1600-and$arm.performance.outputHeight-eq900-and$arm.performance.msaaSamples-eq4-and$arm.performance.activeUpscaler-eq0) 'Actual renderer must be native1600x900,4xMSAA,no upscaler'
        $path=Join-Path $out ($pose.name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(15)
        while(!(Test-Path -LiteralPath $path)-or(Get-Item -LiteralPath $path).Length-eq0){Health;Require ([DateTime]::UtcNow-lt$until) 'Screenshot deadline';Start-Sleep -Milliseconds 100};$arm.screenshot=$path
        SaveReceipt $result $receiptPath
    }
    $first=$result.poses[0].selection;$last=$result.poses[2].selection
    Require ($first.publication.source-eq$last.publication.source-and$first.publication.covered-eq$last.publication.covered-and$first.publication.selected-eq$last.publication.selected-and$first.installed.created-eq$last.installed.created-and(($first.residency.ids|Sort-Object)-join',')-ceq(($last.residency.ids|Sort-Object)-join',')) 'Returning to same city pose changed deterministic source/selection/residency counts'
    $result.returnSelectionConsistent=$true;$result.cityRegionRetiredAtMamer=$true
    $null=Send @{cmd='exit'};$null=$p.WaitForExit(10000);Require ($p.HasExited-and$p.ExitCode-eq0) 'Clean owned-process exit deadline';$result.exitCode=$p.ExitCode;$result.cleanTeardown=$true
    Require (!(Select-String -LiteralPath @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error|Cannot load --test-world' -Quiet)) 'GPU/engine runtime error'
    foreach($line in Get-Content -LiteralPath $log){$pub=ParsePublication $line;if($pub){Require ([string]::IsNullOrEmpty($pub.error)) ('Worldgen error publication: '+$line)}}
    Require (!(Select-String -LiteralPath $log -Pattern 'Earth Worldgen model unavailable/bounded:|Earth Worldgen model not retained:|downloads [1-9]' -Quiet)) 'Missing selected model or unexpected offline download'
    Require ((Get-Content -LiteralPath $stampPath -Raw).Trim()-ceq$stamp) 'Installed build changed during test'
    foreach($path in $identities.Keys){Require ((Get-FileHash -LiteralPath $path).Hash-ceq$identities[$path]) ('Prepared source changed: '+$path)}
    $result.passed=$true;Write-Host ('Worldgen region residency evidence ready; inspect three images: '+$out)
}catch{$result.error=$_.Exception.Message;throw}finally{
    if($p-and!$p.HasExited){if($writer){try{if($null-ne$heldKey){$writer.WriteLine('{"cmd":"key_up","sc":7}')};$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000);$result.forcedCleanup=$true}}
    if($writer){$writer.Dispose()};if($reader){$reader.Dispose()};if($client){$client.Dispose()}
    foreach($key in $saved.Keys){if($exists[$key]){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}else{Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}}
    SaveReceipt $result $receiptPath
}
