# Diagnostic captures only. Root owns installed execution and visual judgement.
[CmdletBinding()]
param(
 [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
 [ValidateRange(4000,10000)][int]$InitialViewDistance=8000,
 [ValidateRange(1,5)][int]$SettleSeconds=2,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='everon-house-owner',
 [ValidateCount(5,5)][double[]]$ReportedPose=@(5805.63,3597.97,46.35,147.8,6.3),
 [ValidateCount(1,24)][int[]]$CandidateIds=@(6034,6035,6036,6037,6042,6043,6044,6045,6047,7710,31997,25191),
 [ValidateCount(1,4)][int[]]$FarCandidateIds=@(6034,6042,6047,7710),
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$SourceOnly,[switch]$SelfTest
)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
$reported=@($ReportedPose)
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Number($value){Require ($null-ne$value) 'Missing number.';$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require ([double]::IsFinite($n)) 'Nonfinite number.';return $n}
function Model-Key([string]$value){$value.Replace('\','/').ToLowerInvariant()}
function Pose($v){Require ($v.Count-eq5) 'Camera needs X,Z,Y,yaw,elevation.';(@($v|ForEach-Object{(Number $_).ToString('R',$culture)})-join' ')}
function Decode([string]$value){$v=$value.Trim();if($v.StartsWith('"')){Require ($v.Length-ge2-and$v.EndsWith('"')) 'Malformed quoted eval.';return $v.Substring(1,$v.Length-2).Replace('""','"')};ConvertFrom-Json -InputObject $v}
function Transform($frame,$point){Require ($frame.Count-eq12-and$point.Count-eq3) 'Bad source/frame dimensions.';@(for($axis=0;$axis-lt3;$axis++){(Number $frame[9+$axis])+(Number $frame[$axis])*(Number $point[0])+(Number $frame[3+$axis])*(Number $point[1])+(Number $frame[6+$axis])*(Number $point[2])})}
function Aim($eye,$target){$dx=$target[0]-$eye[0];$dy=$target[1]-$eye[1];$dz=$target[2]-$eye[2];Pose @($eye[0],$eye[2],$eye[1],([Math]::Atan2($dx,$dz)*180/[Math]::PI),([Math]::Atan2($dy,[Math]::Sqrt($dx*$dx+$dz*$dz))*180/[Math]::PI))}
function Assert-Owner($o,$fixture,$previous){
 Require ($o.id-eq$fixture.id-and$o.present-is[bool]-and$o.present) 'Authored owner absent.'
 Require ($o.destroyed-is[bool]-and!$o.destroyed-and$o.destroyPhase-eq0-and$o.rawDamage-eq0) 'Authored owner damaged.'
 Require ((Model-Key $o.model)-ceq(Model-Key $fixture.model)) 'Owner model differs from source.'
 Require ($o.visualResident-is[bool]-and$o.visualResident-and$o.hasGeometry-is[bool]-and$o.hasGeometry) 'Owner visual/geometry residency absent.'
 Require ($o.frame.Count-eq12-and(Number $o.radius)-gt0) 'Owner frame/radius absent.'
 for($i=0;$i-lt12;$i++){$null=Number $o.frame[$i];if($previous){Require ($o.frame[$i]-eq$previous.frame[$i]) 'Authored owner frame changed.'}}
 foreach($axis in @(0,2)){Require ([Math]::Abs($o.frame[9+$axis]-$fixture.coarsePredictedPosition[$axis])-lt.01) 'Owner does not bind original source XZ.'}
 # Source coarse lowering is a prediction. Keep actual Y authoritative and
 # record its difference; a suspected placement bug must not be gated away.
}
function Assert-Camera($camera,$pose){
 Require ($camera.position.Count-eq3-and$camera.direction.Count-eq3-and$camera.up.Count-eq3) 'Actual camera vectors absent.'
 $expected=@($pose[0],$pose[2],$pose[1]);$yaw=$pose[3]*[Math]::PI/180;$el=$pose[4]*[Math]::PI/180
 $direction=@(([Math]::Sin($yaw)*[Math]::Cos($el)),([Math]::Sin($el)),([Math]::Cos($yaw)*[Math]::Cos($el)))
 for($i=0;$i-lt3;$i++){Require ([Math]::Abs((Number $camera.position[$i])-$expected[$i])-le.02) 'Actual camera position differs.';Require ([Math]::Abs((Number $camera.direction[$i])-$direction[$i])-le.0002) 'Actual camera direction differs.';$null=Number $camera.up[$i]}
}
function Restore-Environment([string]$key,$value){if($null-eq$value){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$value,'Process')}}
function Pair{
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ($stamp-match'^codex/\S+\s+([a-fA-F0-9]{8,40})\s+') 'Installed branch stamp absent.'
 $hash=$Matches[1];$count=[Math]::Min($hash.Length,$ExpectedCommit.Length);Require ($count-ge8-and$hash.Substring(0,$count).Equals($ExpectedCommit.Substring(0,$count),[StringComparison]::OrdinalIgnoreCase)) 'Installed requested commit differs.'
 @{stamp=$stamp;exe=(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
function Same-Pair($a,$b){foreach($key in @('stamp','exe','dll')){Require ($a[$key]-ceq$b[$key]) 'Installed matched pair changed.'}}
function Refuses([scriptblock]$body){$failed=$false;try{&$body|Out-Null}catch{$failed=$true};Require $failed 'Invalid fixture accepted.'}
function Native-Identity($native){
 # GetSeaLevel is the tide/wave level, recomputed from current sun/moon in
 # Landscape::Simulate after date changes. Record it, but do not confuse it
 # with the immutable native heightfield revision or an authored placement.
 $null=Number $native.seaLevel
 foreach($key in @('readonly','baseline','fineSourceReady','ok')){Require ($native.$key-is[bool]) ('Native boolean absent: '+$key)}
 foreach($key in @('range','spacing','heightRevision')){$null=Number $native.$key}
 [ordered]@{readonly=$native.readonly;baseline=$native.baseline;fineSourceReady=$native.fineSourceReady;range=$native.range;spacing=$native.spacing;heightRevision=$native.heightRevision;ok=$native.ok}
}
function Same-Physical($a,$b){
 Require ($a.time-eq$b.time) 'Paused simulation clock changed.'
 Require (((Native-Identity $a.native)|ConvertTo-Json -Compress)-ceq((Native-Identity $b.native)|ConvertTo-Json -Compress)) 'Camera changed native terrain identity/revision.'
 foreach($key in @('mud','sand','snow')){Require (($a[$key]|ConvertTo-Json -Depth 6 -Compress)-ceq($b[$key]|ConvertTo-Json -Depth 6 -Compress)) ('Camera changed physical source/history: '+$key)}
}
if($SelfTest){
 $bottomRows=@(@{world=@(7000,121,2300)},@{world=@(7001,122,2301)})
 Require (($bottomRows|ForEach-Object{$_.world[1]}|Measure-Object -Minimum).Minimum-eq121) 'Foundation height extraction flattened world points.'
 $probeOwner=@{x=7170.;y=126.;z=2370.};$probeFixture=@{visualTopY=132.;coarsePredictedPosition=@(7170.,125.,2370.)}
 $probeTop=@($probeOwner.x,($probeFixture.visualTopY+($probeOwner.y-$probeFixture.coarsePredictedPosition[1])),$probeOwner.z)
 Require (($probeTop-join',')-ceq'7170,133,2370') 'Roof target mixed arithmetic and array construction.'
 Require ((Pose $reported)-ceq'5805.63 3597.97 46.35 147.8 6.3') 'Camera axis order changed.'
 Require (((Transform @(0,0,-1,0,1,0,1,0,0,10,20,30) @(2,3,4))-join',')-ceq'14,23,28') 'Actual column frame transform changed.'
 Require ((Aim @(1,2,3) @(1,2,13))-ceq'1 3 2 0 0') 'Aim changed X,Z,Y camera order.'
 $yaw=147.8*[Math]::PI/180;$el=6.3*[Math]::PI/180;$camera=@{position=@(5805.63,46.35,3597.97);direction=@(([Math]::Sin($yaw)*[Math]::Cos($el)),([Math]::Sin($el)),([Math]::Cos($yaw)*[Math]::Cos($el)));up=@(0,1,0)};Assert-Camera $camera $reported
 $bad=$camera.Clone();$bad.position=@(5805.63,3597.97,46.35);Refuses {Assert-Camera $bad $reported}
 $fixture=@{id=1328;model='data3d\kostel3.p3d';coarsePredictedPosition=@(7362.711,134.233,4772.176)}
 $owner=@{id=1328;present=$true;destroyed=$false;destroyPhase=0;rawDamage=0;visualResident=$true;hasGeometry=$true;model=$fixture.model;radius=25;frame=@(1,0,0,0,1,0,0,0,1,7362.711,134.233,4772.176)};Assert-Owner $owner $fixture $owner
 $bad=$owner.Clone();$bad.model='data3d\kostel.p3d';Refuses {Assert-Owner $bad $fixture $owner}
 $bad=$owner.Clone();$bad.rawDamage=.1;Refuses {Assert-Owner $bad $fixture $owner}
 $bad=$owner.Clone();$bad.frame=@($owner.frame);$bad.frame[10]+=.001;Refuses {Assert-Owner $bad $fixture $owner}
 $suspect=$owner.Clone();$suspect.frame=@($owner.frame);$suspect.frame[10]+=3;Assert-Owner $suspect $fixture $null # do not reject suspected floating Y
 Refuses {Number 'NaN'};Refuses {Number $null}
 $native=@{readonly=$true;baseline=$false;fineSourceReady=$true;range=2048;spacing=6.25;heightRevision=6;seaLevel=2.307034;ok=$true}
 $physical=@{time=8.636;native=$native;mud=@{revision=2;chunks=0};sand=@{revision=2;chunks=0};snow=@{depth=0;chunks=0}}
 $changed=$physical.Clone();$changed.native=$native.Clone();$changed.native.seaLevel=2.982092;Same-Physical $physical $changed
 foreach($key in @('range','spacing','heightRevision')){$bad=$physical.Clone();$bad.native=$native.Clone();$bad.native[$key]+=1;Refuses {Same-Physical $physical $bad}}
 $bad=$physical.Clone();$bad.native=$native.Clone();$bad.native.baseline=$true;Refuses {Same-Physical $physical $bad}
 $bad=$physical.Clone();$bad.native=$native.Clone();$bad.native.seaLevel='NaN';Refuses {Same-Physical $physical $bad}
 $bad=$physical.Clone();$bad.time+=.001;Refuses {Same-Physical $physical $bad}
 $bad=$physical.Clone();$bad.mud=@{revision=3;chunks=0};Refuses {Same-Physical $physical $bad}
 $parseErrors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$null,[ref]$parseErrors);Require ($parseErrors.Count-eq0) 'Fixture has syntax errors.'
 $requests=@($ast.FindAll({param($node) $node-is[Management.Automation.Language.HashtableAst]},$true)|ForEach-Object{$_.Extent.Text}|Where-Object{$_-match"cmd='stream_existing_(move|damage)'"})
 Require ($requests.Count-eq0) 'Runner mutates authored owners.'
 Write-Output 'Everon camera/source/frame falsifiers PASS; no game, profile, GPU or compiler.';return
}
Require ([bool]$ExpectedCommit) 'Supply installed -ExpectedCommit.'
foreach($value in $reported){$null=Number $value}
Require (($CandidateIds | Select-Object -Unique).Count-eq$CandidateIds.Count-and($CandidateIds | Measure-Object -Minimum).Minimum-ge0) 'Candidate IDs must be unique and nonnegative.'
Require (($FarCandidateIds | Select-Object -Unique).Count-eq$FarCandidateIds.Count) 'Far candidate IDs must be unique.'
foreach($id in $FarCandidateIds){Require ($id-in$CandidateIds) 'Every far candidate must belong to the source census.'}
if(!$SourceOnly){Require ([bool]$env:LOCK_OWNER) 'Invoke with scripts/with-game-lock.sh.';Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve existing game owner.'}
$before=Pair
$output=Join-Path $root ('build/everon-placement/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6));New-Item -ItemType Directory -Force $output|Out-Null
$result=[ordered]@{status='prepared';installed=$before;sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;captures=@();owners=@();foundation=@();error=$null;
 scope='Actual reported/targeted/near camera and immutable authored owners, bounded native support samples. No authored owner hide/move/damage, render-height readback, exact selected-pixel owner LOD or appearance acceptance.'}
$sourcePy=Join-Path $output 'stock-foundation.py'
$python=@'
import hashlib, importlib.util, json, math, sys, struct
from pathlib import Path
sys.dont_write_bytecode=True
repo,game,out=map(Path,sys.argv[1:4])
spec=importlib.util.spec_from_file_location('stock',repo/'scripts/physics/inspect_stock_corpse_rig.py')
stock=importlib.util.module_from_spec(spec);spec.loader.exec_module(stock)
import numpy as np
wrp=game/'Worlds/eden.wrp';raw=wrp.read_bytes()
expected='5a2550b27612fc4c8ccdec55ce2555c1ed23114d46f8b687ce5fb8b273515a79'
if hashlib.sha256(raw).hexdigest()!=expected:raise ValueError('installed original Everon differs')
r=stock.Reader(raw)
if r.take(8)!=b'OPRW'+struct.pack('<I',2):raise ValueError('original OPRW2 required')
r.decompress(262144);r.decompress(65536);r.take(r.count()*12);r.decompress(131072);r.decompress(262144)
grid=np.frombuffer(r.decompress(262144),dtype='<f4').reshape(256,256)
if not np.isfinite(grid).all():raise ValueError('nonfinite source terrain')
for _ in range(r.count()):r.string();r.take(1)
names=[r.string() for _ in range(r.count())];placements=[];start=r.pos
while True:
    ident=r.value('i')[0]
    if ident<0:break
    index=r.value('i')[0];frame=np.array(r.value('12f')).reshape(4,3)
    if not 0<=index<len(names) or not np.isfinite(frame).all():raise ValueError('invalid complete source tail')
    placements.append((ident,names[index],frame))
r.finish()
if len(placements)!=56740 or len(names)!=264 or len({v[0] for v in placements})!=54466:raise ValueError('original identity envelope differs')
def height(x,z):
    gx,gz=x/50,z/50;ix,iz=math.floor(gx),math.floor(gz);u,v=gx-ix,gz-iz
    if not 0<=ix<255 or not 0<=iz<255:raise ValueError('source sample outside grid')
    a,b,c,d=grid[iz,ix],grid[iz,ix+1],grid[iz+1,ix],grid[iz+1,ix+1]
    return float(a+u*(b-a)+v*(c-a) if u+v<=1 else d+(1-u)*(c-d)+(1-v)*(b-d))
archive=game/'Dta/Data3D.pbo';index,_=stock.archive_index(archive);models={};rows=[];fixtures=[]
ids=[int(v) for v in sys.argv[4].split(',')]
if not 1<=len(ids)<=24 or len(set(ids))!=len(ids) or min(ids)<0:raise ValueError('invalid bounded candidate IDs')
for ident in ids:
    matches=[v for v in placements if v[0]==ident]
    if len(matches)!=1:raise ValueError('candidate original ID is ambiguous')
    _,name,frame=matches[0]
    if not name.startswith('data3d\\'):raise ValueError('candidate not original model bank')
    if name not in models:
        found=[v for v in index if v['name'].lower()==name.split('\\',1)[1]]
        if len(found)!=1:raise ValueError('ambiguous stock member')
        with archive.open('rb') as f:payload,stored=stock.read_member(f,found[0])
        model=stock.parse_odol7(payload);lods=[]
        for level,lod in enumerate(model['lods']):
            if lod['resolution']>=900:continue
            points=lod['points'];minimum=min(p[1] for p in points);bottom=[(i,p) for i,p in enumerate(points) if p[1]<=minimum+.1]
            selected=set()
            for axis in (0,2):
                for sign in (-1,1):selected.add(min(bottom,key=lambda v:(sign*v[1][axis],v[0]))[0])
            samples=[{'vertex':i,'point':[float(v) for v in points[i]]} for i in sorted(selected)]
            lods.append({'index':level,'resolution':lod['resolution'],'minimumLocalY':float(minimum),'maximumLocalY':float(points[:,1].max()),'bottomVertices':len(bottom),'samples':samples})
        if not 1<=len(lods)<=8 or any(not 1<=len(l['samples'])<=4 for l in lods):raise ValueError('foundation bounds differ')
        models[name]=(model,{'model':name,'decodedSha256':hashlib.sha256(payload).hexdigest(),'storedSha256':stored,'lods':lods})
    model,facts=models[name];anchor=frame[3]-np.array(model['boundingCenter'])@frame[:3];lower=max(0.0,float(anchor[1])-height(anchor[0],anchor[2]));pos=frame[3].copy();pos[1]-=lower
    top=max(l['maximumLocalY'] for l in facts['lods'])+pos[1]
    fixtures.append({'id':ident,'model':name,'wrpFrame':frame.reshape(12).tolist(),'coarsePredictedPosition':pos.tolist(),'legacyLowering':lower,'boundingCentre':model['boundingCenter'],'visualTopY':float(top)})
    rows.append({'id':ident,**facts})
out.write_text(json.dumps({'wrpSha256':expected,'originalObjectCount':56740,'originalDistinctIds':54466,'sourceHasDuplicateNoncandidateIds':True,'originalFullObjectTailVerified':True,'sourceGridSpacing':50,'objects':fixtures,'rows':rows,'scope':'Exact original source IDs and bounded visual bottom points, coarse placement prediction; no selected pixel owner, runtime placement or distant rendered terrain proof.'},indent=2)+'\n')

'@
[IO.File]::WriteAllText($sourcePy,$python,[Text.UTF8Encoding]::new($false))
& python $sourcePy $root $GameDir (Join-Path $output 'stock-foundation.json') ($CandidateIds -join ',')
if($LASTEXITCODE-ne0){$result.status='failed-source';$result.error='Actual stock foundation source audit failed.';$result|ConvertTo-Json -Depth 18|Set-Content (Join-Path $output 'result.json');throw $result.error}
$result.stockSource=Get-Content (Join-Path $output 'stock-foundation.json') -Raw|ConvertFrom-Json
$fixtures=@($result.stockSource.objects);Require ($fixtures.Count-eq$candidateIds.Count) 'Complete candidate census absent.'
if($SourceOnly){Same-Pair $before (Pair);$result.status='source-only-no-runtime';$result|ConvertTo-Json -Depth 18|Set-Content (Join-Path $output 'result.json');Write-Output $output;return}
$settings=@{WGR_WET_SOIL_DIAGNOSTIC='1';POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';WGR_GRASS='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','WGR_CLOUD_COVERAGE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLE_WETNESS')
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR');$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result.environment=@{settings=$settings;cleared=$clear;inherited=$saved};$result.requestedInitialViewDistance=$InitialViewDistance;$result.reportedPose=$reported
$p=$null;$client=$null;$writer=$null;$reader=$null;$deadline=$null;$log=Join-Path $output 'engine.log'
function Log-Lines{if(!(Test-Path $log)){return @()};$stream=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite-bor[IO.FileShare]::Delete));$r=[IO.StreamReader]::new($stream);try{$r.ReadToEnd().Split("`n")}finally{$r.Dispose()}}
function Health{Require (!$p.HasExited) 'Owned game exited early.';Require ([DateTime]::UtcNow-lt$deadline) 'Everon arm exceeded bounded 420 seconds.';Require (!(Log-Lines|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Cannot load --test-world|StartAutoTest could not boot')) 'Runtime failure log.'}
function Send($request){Health;$json=$request|ConvertTo-Json -Compress;$json|Add-Content (Join-Path $output 'harness.jsonl');$writer.WriteLine($json);$until=[DateTime]::UtcNow.AddSeconds(30);do{Health;Require ([DateTime]::UtcNow-lt$until) 'Harness reply exceeded 30 seconds.';$line=$reader.ReadLine();Require ($null-ne$line) 'Harness closed.';$line|Add-Content (Join-Path $output 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;$reply}
function Eval([string]$code){Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Owners{
 $reply=Send @{cmd='stream_identity_probe';ids=$candidateIds};Require ($reply.objects.Count-eq$candidateIds.Count) 'Complete actual source candidates required.'
 foreach($fixture in $fixtures){$o=@($reply.objects|Where-Object id -eq $fixture.id);Require ($o.Count-eq1) 'Nonunique live owner.';$previous=@($result.owners|Where-Object id -eq $fixture.id);Assert-Owner $o[0] $fixture $(if($previous.Count){$previous[0]}else{$null})};$reply.objects
}
function State{
 $soil=Send @{cmd='dev_wet_soil_diagnostic';action='state'};Require ($soil.mode-eq0) 'Natural fragment path is not mode zero.'
 @{time=(Number (Eval 'time'));camera=$soil.camera;native=(Send @{cmd='dev_terrain_brush';action='state'});mud=(Send @{cmd='dev_mud';action='state'});sand=(Send @{cmd='dev_sand';action='state'});snow=(Send @{cmd='dev_snow';action='state'})}
}
function Capture([string]$name,$pose){
 Require ((Eval ('triFreeFlyPose "'+(Pose $pose)+'"'))-ceq'OK') 'Camera refused.';Start-Sleep -Seconds $SettleSeconds
 $state=State;Same-Physical $result.initialState $state;Assert-Camera $state.camera $pose;$owners=@(Owners);$weather=Send @{cmd='weather_visibility'};$actualView=@{preferred=(Number (Eval 'triGetViewDistance'));tactical=(Number (Eval 'triViewDistance'))}
 $path=Join-Path $output ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
 do{Health;$bytes=$null;if(Test-Path $path){try{$bytes=[IO.File]::ReadAllBytes($path)}catch [IO.IOException]{}};if($bytes-and$bytes.Length-ge36-and[BitConverter]::ToString($bytes[0..7])-ceq'89-50-4E-47-0D-0A-1A-0A'-and($bytes[($bytes.Length-8)..($bytes.Length-5)]-join',')-ceq'73,69,78,68'){break};Require ([DateTime]::UtcNow-lt$until) 'Complete PNG timeout.';Start-Sleep -Milliseconds 100}while($true)
 Require (($bytes[16..19]-join',')-ceq'0,0,5,0'-and($bytes[20..23]-join',')-ceq'0,0,2,208') 'PNG dimensions differ from requested 1280x720.'
 $after=State;Same-Physical $state $after;Assert-Camera $after.camera $pose;$null=Owners
 $result.captures+=@{name=$name;path=$path;sha256=(Get-FileHash $path).Hash;requestedPose=$pose;state=$state;after=$after;owners=$owners;weather=$weather;actualViewDistance=$actualView;scope='Actual unchanged paused camera/owners around complete PNG; screenshot API has no exact submitted-frame/owner-selectedLOD association.'}
}
try{
 foreach($key in $clear){Restore-Environment $key $null};foreach($key in $settings.Keys){Restore-Environment $key $settings[$key]}
 $profile=Join-Path $output 'user';New-Item -ItemType Directory -Force $profile|Out-Null;& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
 $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;')));$env:POSEIDON_USER_DIR=$profile;$result.profileSha256=(Get-FileHash $cfg).Hash
 $mission=Join-Path $root 'tests/perf/missions/parity_field.eden';$result.missionSha256=(Get-FileHash (Join-Path $mission 'mission.sqm')).Hash
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly')+((Pose $reported)-split' ')+@('--log-file',('"'+$log+'"'))
 if($InitialViewDistance){$arguments+=@('--vd',"$InitialViewDistance")};$result.arguments=$arguments;$deadline=[DateTime]::UtcNow.AddSeconds(420)
 Same-Pair $before (Pair);Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.'
 $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $arguments -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt') -PassThru;$null=$p.Handle;$result.pid=$p.Id
 $until=[DateTime]::UtcNow.AddSeconds(120);do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$until) 'Harness startup timeout.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady')-ceq'OK') 'Stock Eden scene not ready.'
 $null=Send @{cmd='exec';code='setAccTime 0;player allowDamage false;0 setRain 0;0 setFog 0;0 setOvercast 0;setDate [1985,6,21,16,0];triSetBrightness 1'}
 $result.initialState=State;Require ($result.initialState.native.readonly-and!$result.initialState.native.baseline) 'Terrain edited before fixture.'
 $result.owners=@(Owners);$result.geometry=@(foreach($fixture in $fixtures){$g=Send @{cmd='diag_geometry';unit=('object '+$fixture.id);draw=$false};Require ((Model-Key $g.lod.name)-ceq(Model-Key $fixture.model)) 'Actual geometry source differs.';Require ($g.lod.min.Count-eq3-and$g.lod.max.Count-eq3-and$g.lod.lods-ge1) 'Actual shape bounds missing.';foreach($v in @($g.lod.min)+@($g.lod.max)){$null=Number $v};@{id=$fixture.id;geometry=$g}});$result.originalRecordedVisibility=Send @{cmd='weather_visibility'};$result.originalRecordedViewDistance=@{preferred=(Number (Eval 'triGetViewDistance'));tactical=(Number (Eval 'triViewDistance'))}
 Capture 'reported-startup-range' $reported
 Require ((Eval 'triSetViewDistance 8000')-match'^OK') 'Distant-range control refused.'
 Capture 'reported-explicit-8000' $reported
 $visibility=$result.captures[-1].weather;Require ($result.captures[-1].actualViewDistance.preferred-ge7999-and$result.captures[-1].actualViewDistance.tactical-ge7999-and$visibility.terrainRange-ge7500-and$visibility.objectRange-ge7500) 'Actual distant ranges insufficient for source candidates.'
 foreach($fixture in $fixtures){
  $owner=@($result.owners|Where-Object id -eq $fixture.id)[0];$source=@($result.stockSource.rows|Where-Object id -eq $fixture.id)[0];$samples=@()
  foreach($lod in $source.lods){foreach($sample in $lod.samples){
   $world=Transform $owner.frame $sample.point;$ground=Send @{cmd='dev_mud';action='sample';x=$world[0];z=$world[2]};Require ($ground.world-match'(?i)(^|[\\/])eden\.wrp$') 'Foundation sample world is not actual Eden.'
   $height=Number $ground.surfaceY;$samples+=@{lodIndex=$lod.index;resolution=$lod.resolution;vertex=$sample.vertex;local=$sample.point;world=$world;actualGround=$ground;nativeGap=$world[1]-$height}
  }}
  Require ($samples.Count-ge1-and$samples.Count-le96) 'Foundation sample bounds changed.'
  $result.foundation+=@{id=$fixture.id;model=$fixture.model;actualFrame=$owner.frame;sourcePositionDifferenceY=$owner.y-$fixture.coarsePredictedPosition[1];samples=$samples;minimumGap=($samples.nativeGap|Measure-Object -Minimum).Minimum;maximumGap=($samples.nativeGap|Measure-Object -Maximum).Maximum;scope='Bounded source-LOD bottom points transformed by actual owner frame against native SurfaceY; neither selected-pixel LOD nor rendered distant terrain.'}
  $min=($samples|ForEach-Object{$_.world[1]}|Measure-Object -Minimum).Minimum;$target=@($owner.x,($min+3.0),$owner.z)
  $eyeX=$owner.x-60.0;$eyeZ=$owner.z;$ground=Send @{cmd='dev_mud';action='sample';x=$eyeX;z=$eyeZ};$nearEye=@($eyeX,((Number $ground.surfaceY)+4.0),$eyeZ)
  Capture ('owner'+$fixture.id+'-near-native-foundation') (Decode ('['+((Aim $nearEye $target)-replace' ', ',')+']'))
  $top=@($owner.x,($fixture.visualTopY+($owner.y-$fixture.coarsePredictedPosition[1])),$owner.z)
  if($fixture.id-in$FarCandidateIds){
   $ray=@();for($step=1;$step-le24;$step++){$t=$step/25.0;$px=$reported[0]+$t*($top[0]-$reported[0]);$pz=$reported[1]+$t*($top[2]-$reported[1]);$py=$reported[2]+$t*($top[1]-$reported[2]);$sample=Send @{cmd='dev_mud';action='sample';x=$px;z=$pz};$ray+=@{fraction=$t;world=@($px,$py,$pz);actualGround=$sample;terrainAboveSample=(Number $sample.surfaceY)-$py}}
   $result.foundation[-1].sampledNativeSightline=@{samples=$ray;maximumTerrainOverLine=($ray.terrainAboveSample|Measure-Object -Maximum).Maximum;scope='24 actual native SurfaceY samples along camera-to-predicted-roof line. Not a continuous terrain intersection, object occlusion test, raster height or selected pixel proof.'}
  }
  if($fixture.id-in$FarCandidateIds){Capture ('owner'+$fixture.id+'-targeted-far') (Decode ('['+((Aim @($reported[0],$reported[2],$reported[1]) $top)-replace' ', ',')+']'))}
 }
 Capture 'reported-8000-return' $reported
 $result.finalGeometry=@(foreach($fixture in $fixtures){$g=Send @{cmd='diag_geometry';unit=('object '+$fixture.id);draw=$false};Require ((Model-Key $g.lod.name)-ceq(Model-Key $fixture.model)) 'Final geometry source differs.';@{id=$fixture.id;geometry=$g}})
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Owned game did not exit normally.'
 $result.lifecycle=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$meta=Join-Path $output 'lifecycle.json';$result.lifecycle|ConvertTo-Json|Set-Content $meta
 & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $meta -LogPath $log;Require ($LASTEXITCODE-eq0) 'Lifecycle helper refused capture.';$result.after=Pair;Same-Pair $before $result.after;$result.status='captured-identification-and-render-support-unresolved';$p=$null
}catch{$result.status='failed';$result.error=$_.Exception.Message;throw}
finally{
 if($p-and!$p.HasExited-and$writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(20000)}catch{$result.cleanupError=$_.Exception.Message}}
 if($client){$client.Dispose()};if($p-and!$p.HasExited){$result.status='failed';$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue}
 foreach($key in $keys){Restore-Environment $key $saved[$key]};$result|ConvertTo-Json -Depth 22|Set-Content (Join-Path $output 'result.json');Write-Output $output
}
