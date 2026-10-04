# Root runtime owner invokes through with-game-lock.sh against the installed paired build.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='procedural-sinkhole',
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [string]$ModDir=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Pair {
    $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
    Require ($stamp -match ('\b'+[regex]::Escape($ExpectedCommit)+'\b')) 'Installed commit differs.'
    return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{
        $f=Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}
    })}
}
$before=Pair
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
if(!$ModDir){$ModDir=Join-Path $root 'build/sinkhole-test/@OP_SinkholeTest'}
$ModDir=(Resolve-Path -LiteralPath $ModDir).ProviderPath
$packs=@('ugcave.pbo','ugtest.pbo')|ForEach-Object{
    $f=Get-Item -LiteralPath (Join-Path $ModDir ('AddOns/'+$_));Require ($f.Length -gt 1000) 'Empty cave pack.'
    @{name=$_;path=$f.FullName;bytes=$f.Length;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash}
}
$out=Join-Path $root ('build/sinkhole-content/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force -Path $out|Out-Null
$saved=@{};$keys=@('POSEIDON_USER_DIR','POSEIDON_AUTOMATIC_RAGDOLL','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_GRASS','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_TEMPORAL','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;packs=$packs;arms=@();scope='Actual optional mission config/model/hole/native roadway and roof samples plus screenshots. No AI traversal, rifle combat, Box3D underground collision, exact GPU pixel ownership, swim content or complete cave regeneration acceptance.'}
$p=$null;$client=$null;$writer=$null
function Send($command){
    Require ($p -and !$p.HasExited) 'Owned game exited unexpectedly.'
    $wire=$command|ConvertTo-Json -Depth 12 -Compress;$writer.WriteLine($wire)
    $wire|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl')
    do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
    $line|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl');Require $reply.ok $line;return $reply
}
function Eval([string]$code){
    $v=([string](Send @{cmd='eval';code=$code}).result).Trim()
    if($v.StartsWith('"')){Require $v.EndsWith('"') 'Incomplete SQF string.';return $v.Substring(1,$v.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $v -NoEnumerate
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Fmt($v){$n=[double]$v;Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite fixture value.';return $n.ToString('R',[Globalization.CultureInfo]::InvariantCulture)}
function State([double]$x,[double]$y,[double]$z){
    $s=Send @{cmd='dev_cave_editor';action='state';x=$x;y=$y;z=$z}
    Require ($s.worldName.Replace('\','/').ToLowerInvariant() -match '(^|/)eden\.wrp$') 'Not original Everon.'
    Require ($s.count -eq 0) 'Mission-local editor excavation unexpectedly present.'
    return $s
}
function Capture([string]$name,$eye,$dir){
    Exec ('triSetView ['+((@($eye)+@($dir)|ForEach-Object{Fmt $_}) -join ',')+']')
    Start-Sleep -Milliseconds 800
    $path=Join-Path $armOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(15)
    while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow -lt $until) 'Screenshot deadline.';Start-Sleep -Milliseconds 100}
    Require ((Get-Item -LiteralPath $path).Length -gt 4096) 'Screenshot is empty.'
    $arm.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;eyeXYZ=$eye;directionXYZ=$dir}
    Exec 'triClearView'
}
try {
    foreach($key in $keys){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    $env:POSEIDON_AUTOMATIC_RAGDOLL='0';$env:POSEIDON_SNOWLINE='off';$env:POSEIDON_SNOW_TEST_DEPTH='0';$env:WGR_GRASS='0';$env:WGR_TEMPORAL='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='0.6';$env:WGR_TONEMAP='1'
    foreach($fixture in @(
        @{name='cave';mission='ugcave-test.Eden';variable='cave';class='UgCave';shape='ugcave/ugcave.p3d';x=4870.;z=11305.;depth=5.;entranceZ=11324.;entranceY=-0.3},
        @{name='basement';mission='ugbasement-test.Eden';variable='ub';class='UgBasement';shape='ugtest/ugbasement.p3d';x=9698.;z=1622.;depth=3.;entranceZ=1630.;entranceY=-0.3}
    )) {
        Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Another game appeared; preserving it.'
        Require ((Pair|ConvertTo-Json -Depth 5 -Compress) -ceq ($before|ConvertTo-Json -Depth 5 -Compress)) 'Installed pair changed.'
        $armOut=Join-Path $out $fixture.name;$profile=Join-Path $armOut 'user';New-Item -ItemType Directory -Force -Path $profile|Out-Null
        $env:POSEIDON_USER_DIR=$profile
        [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $log=Join-Path $armOut 'engine.log';$arm=@{fixture=$fixture;log=$log;passed=$false;captures=@{}};$result.arms+=,$arm
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
        $mission=Join-Path $root ('dev-missions/'+$fixture.mission)
        $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--mod',('"'+$ModDir+'"'),'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
        $arm.arguments=$args
        $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru
        $null=$p.Handle;$arm.pid=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(120)
        do{
            Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new()
            try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
            if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness startup deadline.';Start-Sleep -Milliseconds 250}
        }while(!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        while((Eval 'triSceneReady') -cne 'OK'){Require ([DateTime]::UtcNow -lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 250}
        # Let the supplied mission init create/configure its authored object.
        Start-Sleep -Seconds 8
        Exec 'setAccTime 0;player allowDamage false;0 setRain 0;0 setFog 0;0 setOvercast 0;setDate [1985,6,21,16,0]'
        $arm.inspect=Send @{cmd='diag_inspect';unit=$fixture.variable}
        Require ($arm.inspect.cls -ceq $fixture.class) 'Mission object class/config missing.'
        $arm.geometry=Send @{cmd='diag_geometry';unit=$fixture.variable;draw=$false}
        $shape=$arm.geometry.lod.name.Replace('\','/').ToLowerInvariant()
        Require ($shape.EndsWith($fixture.shape)) 'Actual model differs from authored fixture.'
        Require ($arm.geometry.lod.geometry -and $arm.geometry.lod.fire -and $arm.geometry.lod.memory -and $arm.geometry.lod.lods -ge 6) 'Required model LODs missing.'
        $origin=Eval ('getPosASL '+$fixture.variable);Require ($origin.Count -eq 3) 'Object position missing.';$h=[double]$origin[2];$arm.originXZY=$origin
        Require ([Math]::Abs($origin[0]-$fixture.x) -lt .1 -and [Math]::Abs($origin[1]-$fixture.z) -lt .1) 'Mission object moved from supplied site.'
        $floor=$h-$fixture.depth
        $arm.floor=State $fixture.x ($floor+.4) $fixture.z
        $arm.entrance=State $fixture.x ($h+$fixture.entranceY) $fixture.entranceZ
        $arm.roof=State $fixture.x ($h+.3) $fixture.z
        Require ($arm.floor.atHeight -and $arm.floor.inFootprint -and $arm.floor.terrainY -gt $floor+2) 'Native underground hole was not admitted.'
        Require ([Math]::Abs($arm.floor.roadY-$floor) -lt .25 -and [Math]::Abs($arm.floor.cameraFloorY-$floor) -lt .25) 'Native roadway/camera floor differs from authored floor.'
        Require ($arm.entrance.inFootprint -and $arm.entrance.drawRecords -gt 0) 'Authored entrance/drawn hole records missing.'
        # Report roof values without assuming generated editor bounded-header semantics.
        $arm.roofInterpretation='Native terrain/roadway/camera samples above source roof; authored legacy hole selections differ from editor-generated bounded headers.'
        Capture 'entrance' @($fixture.x,($h+2),($fixture.entranceZ+8)) @(0,-.15,-1)
        Capture 'inside' @($fixture.x,($floor+1.6),$fixture.z) @(0,0,1)
        Capture 'above' @($fixture.x,($h+12),$fixture.z) @(0,-1,.001)
        $arm.finalGeometry=Send @{cmd='diag_geometry';unit=$fixture.variable;draw=$false}
        Require ($arm.finalGeometry.lod.name -ceq $arm.geometry.lod.name) 'Model changed during captures.'
        $arm.finalFloor=State $fixture.x ($floor+.4) $fixture.z
        Require ($arm.finalFloor.heightRevision -eq $arm.floor.heightRevision -and [Math]::Abs($arm.finalFloor.terrainY-$arm.floor.terrainY) -lt .0001) 'Native terrain source changed.'
        $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game did not exit normally.'
        Require ([bool](Select-String -LiteralPath $log -SimpleMatch 'Shutdown complete' -Quiet)) 'Normal shutdown receipt missing.'
        Require (!(Select-String -LiteralPath $log -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Engine/GPU failure in log.'
        $arm.exitCode=$p.ExitCode;$arm.passed=$true;$p=$null;$client.Dispose();$client=$null;$writer=$null
    }
    $result.after=Pair
    Require (($result.after|ConvertTo-Json -Depth 5 -Compress) -ceq ($before|ConvertTo-Json -Depth 5 -Compress)) 'Installed pair changed.'
    foreach($pack in $packs){Require ((Get-FileHash -LiteralPath $pack.path).Hash -ceq $pack.sha256) 'Optional content pack changed.'}
    $result.passed=$true
}catch{$result.error=$_.Exception.Message;throw}
finally {
    if($p -and !$p.HasExited -and $writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(20000)}catch{$result.cleanupError=$_.Exception.Message}}
    if($client){$client.Dispose()}
    if($p -and !$p.HasExited){$result.passed=$false;$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue}
    foreach($key in $keys){if($null -eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
    $result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath (Join-Path $out 'result.json');Write-Output $out
}
