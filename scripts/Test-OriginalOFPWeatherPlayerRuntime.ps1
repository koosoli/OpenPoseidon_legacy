# Original stock maps, actual controlled standard US soldier. Root owns runtime.
[CmdletBinding()]
param(
 [ValidateSet('eden','noe','abel','cain')][string[]]$Maps=@('eden','noe','abel','cain'),
 [ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit='',
 [string]$SourceCensus='',
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='original-player',
 [ValidateRange(30,120)][int]$WetDeadlineSeconds=75,
 [ValidateRange(.3,.95)][double]$MinimumMudWetness=.3,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$NativeHeightFixture,
 [switch]$ContactViews,
 [switch]$SandSnapshotTrace,
 [switch]$UniformLightingViews,
 [switch]$TerrainWetnessTrace,
 [switch]$SelfTest
)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Number($v){$n=[double]::Parse([string]$v,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n)-and ![double]::IsInfinity($n)) 'Nonfinite fixture number.';return $n}
function Mud-WetReady($state,[bool]$hasMud,[double]$minimum){
 $null=Number $minimum;Require ($minimum -ge .3 -and $minimum -le .95) 'Requested natural mud wetness outside[.3,.95].'
 foreach($value in @($state.uniform,$state.weather.liquidRain)){Require ((Number $value) -ge 0 -and (Number $value) -le 1) 'Invalid actual cloth/liquid wetting state.'}
 if($hasMud){$mud=Number $state.mud.wetness;Require ($mud -ge 0 -and $mud -le 1) 'Invalid actual GMud wetness.';if($mud -lt $minimum){return $false}}
 return $state.uniform -ge .3 -and $state.weather.liquidRain -ge .3
}
function Terrain-WetnessRows($lines){
 $rows=@();$grammar='Wgpu terrain puddle weather: time=([-+\d.eE]+) rain=([-+\d.eE]+) accumulated=([-+\d.eE]+) uploaded=([-+\d.eE]+)\r?$'
 foreach($line in $lines){if($line.Contains('Wgpu terrain puddle weather:')){
  Require ($line -cmatch $grammar) 'Unknown/incomplete existing terrain weather diagnostic.'
  $m=$Matches.Clone();$row=@{time=(Number $m[1]);queuedRainStrength=(Number $m[2]);accumulated=(Number $m[3]);queuedPad1=(Number $m[4]);line=$line}
  Require ($row.time -ge 0) 'Negative terrain presentation clock.'
  foreach($field in @('queuedRainStrength','accumulated','queuedPad1')){Require ($row[$field] -ge 0 -and $row[$field] -le 1) 'Existing terrain telemetry outside[0,1].'}
  $rows+=,$row
 }};return $rows
}
function Key([string]$v){return $v.Replace('\','/').ToLowerInvariant()}
function Contact-Poses([double]$x,[double]$z,[double]$height){
 return @(@{name='low';pose=@(($x-2),$z,($height+.6),90,-16.699244)},@{name='top';pose=@(($x+.2),$z,($height+8),270,-88.567904)})
}
function Uniform-HeadingDifference([double]$actual,[double]$anchor){
 $null=Number $actual;$null=Number $anchor
 Require ($actual -ge 0 -and $actual -le 360 -and $anchor -ge 0 -and $anchor -le 360) 'Actual comparison heading outside compass range.'
 $delta=[Math]::Abs($actual-$anchor)
 return [Math]::Min($delta,360-$delta)
}
function Assert-UniformHeading([double]$actual,[double]$anchor){
 Require ((Uniform-HeadingDifference $actual $anchor) -lt .01) 'Actual anchored stock comparison heading changed.'
}
function Uniform-View($position,[double]$heading){
 Require (@($position).Count -eq 3) 'Uniform ASL position needs three coordinates.'
 $x=Number $position[0];$z=Number $position[1];$y=Number $position[2]
 $null=Uniform-HeadingDifference $heading $heading
 # getDir is atan2(Direction.X,Direction.Z): forward=(sin,cos),
 # right=(cos,-sin). Use the settled dry heading, not an earlier setDir request.
 # A low front-quarter camera lies 2.5m forward and 1.5m right, raw X,Y,Z.
 $angle=$heading*[Math]::PI/180;$sin=[Math]::Sin($angle);$cos=[Math]::Cos($angle)
 $dx=2.5*$sin+1.5*$cos;$dz=2.5*$cos-1.5*$sin
 return @(($x+$dx),($y+.9),($z+$dz),(-$dx),.35,(-$dz))
}
function Assert-UniformSun([double]$hour,[double]$factor,[bool]$night){
 $null=Number $hour;$null=Number $factor
 if($night){Require ($hour -ge 0 -and $hour -lt .01 -and $factor -le .01 -and $factor -ge 0) 'Actual midnight/sun night lighting is absent.'}
 else{Require ([Math]::Abs($hour-16) -lt .01 -and $factor -ge .99 -and $factor -le 1) 'Actual daytime/sun daylight lighting is absent.'}
}
function Lamp-Live($lines){
 $rows=@($lines|Where-Object{$_ -match 'LAMP-001: \d+ lamp simulations since start, \d+ of them switched on, (\d+) lamp lights live in the scene'})
 Require ($rows.Count -gt 0) 'Actual stock lamp scene-light receipt absent.'
 Require ($rows[-1] -match ', (\d+) lamp lights live in the scene') 'Malformed lamp live-count receipt.'
 return [int]$Matches[1]
}
function Model-Stem([string]$debug){
 # EntityAI without a commander wraps Object's shape name in its pointer and
 # locality marker. This grammar is for static model proof, never unit IDs.
 Require ($debug -match '^(?:(?:0[xX])?[0-9A-Fa-f]+# )?(?:NOID|\d+:)\s+(.+?)(?: REMOTE)?$' -and $Matches[1] -notmatch '<no shape>') 'Actual object has no identifiable model shape.'
 return $Matches[1].ToLowerInvariant()
}
function Decode([string]$v){$v=$v.Trim();if($v.StartsWith('"')){Require $v.EndsWith('"') 'Bad SQF string.';return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json $v -NoEnumerate}
function Restore-Env([string]$key,$v){if($null-eq $v){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$v,'Process')}}
function Assert-Tuple($s,$expected,[string]$world,[string]$kind){
 Require ($s.ok -eq $true -and $s.enabled -eq $true) 'Physical sample is unavailable/disabled.'
 foreach($f in @('x','z','offset','surfaceY','surfaceDx','surfaceDz')){$null=Number $s.$f}
 if($kind-eq 'sand'){Require ($s.offset-ge -.15001 -and $s.offset-le .02001) 'Actual sand geometry exceeds signed contract.'}
 else{Require ($s.offset-ge -.35001 -and $s.offset-le .00001) 'Actual mud geometry exceeds signed contract.'}
 Require ((Key $s.world)-match ('(^|/)'+$world+'\.wrp$')) 'Runtime original world differs from selected mission.'
 Require ((Key $s.texture)-ceq (Key $expected.texture)) 'Actual lane texture differs from audited source.'
 # Config class identity is case-insensitive; runtime canonicalizes Default.
 # Authored file/sound/character values and the exact texture remain strict.
 $match=@($expected.quadrants|Where-Object{[string]$_.surfaceClass -ieq [string]$s.surfaceClass -and [string]$_.files -ceq [string]$s.files -and [string]$_.sound -ceq [string]$s.sound -and [string]$_.character -ceq [string]$s.character})
 Require ($match.Count -gt 0) 'Actual class/files/sound/character differs from audited source.'
 if($kind -in @('sand','mud')){Require ($s.sourceEligible -is [bool] -and $s.sourceEligible) 'Actual source refuses requested soft contact.'}
}
function Boot-Rows($lines,[string]$kind){
 $rows=@();foreach($line in $lines){
  if($line -match ('PLAYER_GROUND_BOOT time=([-+\d.eE]+) manual=true .+ x=([-+\d.eE]+) y=([-+\d.eE]+) z=([-+\d.eE]+) .+ '+$kind+'Changed=true')){
   $rows+=@{time=(Number $Matches[1]);x=(Number $Matches[2]);y=(Number $Matches[3]);z=(Number $Matches[4]);line=$line}
  }
 };return @($rows)
}
function Assert-Body($lines,[double]$minimumTime=-1){
 $queue=@($lines|Where-Object{$_ -match 'PLAYER_GROUND_QUEUE time=([-+\d.eE]+) camera=2 .+resident=true invisible=false .+model=data3d\\mc vojakw2\.p3d' -and (Number $Matches[1])-ge $minimumTime})
 $draw=@($lines|Where-Object{$_ -match 'PLAYER_GROUND_DRAW time=([-+\d.eE]+) camera=2 .+model=data3d\\mc vojakw2\.p3d .+dead=false hidden=0\.000' -and (Number $Matches[1])-ge $minimumTime})
 $section=@($lines|Where-Object{$_ -match 'PLAYER_GROUND_SECTION camera=2 .+model=data3d\\mc vojakw2\.p3d .+primaryMesh=true .+indices=[1-9]\d* '})
 Require ($queue.Count -gt 0 -and $draw.Count -gt 0 -and $section.Count -gt 0) 'Actual external player queue/draw/primary body sections absent or hidden.'
 return @{queue=@($queue|Select-Object -Last 3);draw=@($draw|Select-Object -Last 3);section=@($section|Select-Object -Last 8);scope='Actual source draw evidence; body pixels still require inspected PNGs'}
}
function Assert-Cloth($lines){
 $rows=@();foreach($line in $lines){
  if($line -match 'UNIFORM_CLOTH model=data3d\\mc vojakw2\.p3d texture=merged\\00007mc_vojakw2\.paa kind=2 enabled=(true|1) wet=([-+\d.eE]+) encoded=([-+\d.eE]+)'){
   $wet=Number $Matches[2];$encoded=Number $Matches[3];if($wet -ge .2){Require ([Math]::Abs($encoded-(2+$wet)) -le .00015) 'Stock cloth encoded upload differs from actual wet band.';$rows+=$line}
  }
 };Require ($rows.Count -gt 0) 'Wet actual standard US body atlas upload absent.';return @($rows|Select-Object -Last 4)
}
function Pair{
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
 Require ($stamp -match '^codex/streaming-residency\s+([a-fA-F0-9]{8,40})\s+') 'Installed source stamp absent.'
 $commit=$Matches[1];$n=[Math]::Min($commit.Length,$ExpectedCommit.Length)
 Require ($n -ge 8 -and $commit.Substring(0,$n).Equals($ExpectedCommit.Substring(0,$n),[StringComparison]::OrdinalIgnoreCase)) 'Installed pair differs from requested commit.'
 return [ordered]@{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o');sha256=(Get-FileHash -LiteralPath $f.FullName).Hash}})}
}
function Same-Pair($a,$b){Require (($a|ConvertTo-Json -Depth 8 -Compress)-ceq ($b|ConvertTo-Json -Depth 8 -Compress)) 'Installed source pair changed.'}
function Test-Helpers{
 $wetFixture=@{uniform=.3;weather=@{liquidRain=.3};mud=@{wetness=.3}}
 Require (Mud-WetReady $wetFixture $true .3) 'Default natural wetting threshold changed.'
 Require (!(Mud-WetReady $wetFixture $true .8)) 'Intermediate GMud wetness admitted as high wetness.'
 $wetFixture.mud.wetness=.8;Require (Mud-WetReady $wetFixture $true .8) 'Actual high natural mud wetness refused.'
 $wetFixture.mud.wetness=0;Require (Mud-WetReady $wetFixture $false .8) 'Unsupported-map cloth control demanded invented mud admission.'
 $wetFixture.mud.wetness=1;$wetFixture.weather.liquidRain=.2;Require (!(Mud-WetReady $wetFixture $true .8)) 'High retained GMud substituted for actual liquid rain.'
 foreach($minimum in @(.2,1,[double]::NaN)){$failed=$false;try{Mud-WetReady $wetFixture $true $minimum}catch{$failed=$true};Require $failed 'Invalid natural mud threshold accepted.'}
 $trace='Wgpu terrain puddle weather: time=35.001 rain=0.500 accumulated=0.4523 uploaded=0.4523'+[char]13
 $rows=@(Terrain-WetnessRows @($trace));Require ($rows.Count -eq 1 -and $rows[0].accumulated -eq .4523 -and $rows[0].queuedPad1 -eq .4523) 'Existing CRLF queued terrain weather receipt changed.'
 $rows=@(Terrain-WetnessRows @($trace.Replace('uploaded=0.4523','uploaded=0.0000')));Require ($rows[0].queuedPad1 -eq 0 -and $rows[0].accumulated -gt 0) 'Master-disabled queued coat was fabricated as wet.'
 foreach($bad in @($trace.Replace('rain=0.500','rain=NaN'),$trace.Replace('uploaded=0.4523','uploaded=1.1000'))){$failed=$false;try{Terrain-WetnessRows @($bad)}catch{$failed=$true};Require $failed 'Malformed/out-of-range queued terrain receipt accepted.'}
 $view=Uniform-View @(10,20,30) 90
 Require ($view.Count -eq 6 -and $view[0] -eq 12.5 -and $view[1] -eq 30.9 -and $view[2] -eq 18.5) 'Uniform camera reordered SQF ASL coordinates.'
 foreach($heading in @(0,52.0466,90,180,270,359.995)){
  $v=Uniform-View @(10,20,30) $heading;$angle=$heading*[Math]::PI/180;$sin=[Math]::Sin($angle);$cos=[Math]::Cos($angle)
  $dx=$v[0]-10;$dz=$v[2]-20
  Require ([Math]::Abs($dx*$sin+$dz*$cos-2.5) -lt .0000001 -and [Math]::Abs($dx*$cos-$dz*$sin-1.5) -lt .0000001) 'Camera does not follow actual actor front-quarter heading.'
  Require ([Math]::Abs($v[0]+$v[3]-10) -lt .0000001 -and [Math]::Abs($v[2]+$v[5]-20) -lt .0000001 -and [Math]::Abs($v[1]+$v[4]-31.25) -lt .0000001) 'Heading-derived raw camera does not aim at the same stock body anchor.'
 }
 Assert-UniformHeading 52.0466 52.0466;Assert-UniformHeading 359.999 0
 foreach($pair in @(@(52.0466,90),@(52.0466,53),@([double]::NaN,90),@(361,1))){
  $failed=$false;try{Assert-UniformHeading $pair[0] $pair[1]}catch{$failed=$true};Require $failed 'Changed/nonfinite/out-of-range anchored heading accepted.'
 }
 Assert-UniformSun 16 1 $false;Assert-UniformSun 0 0 $true
 foreach($v in @(@(16,1,$true),@(0,0,$false),@(0,.5,$true),@(16,[double]::NaN,$false))){$failed=$false;try{Assert-UniformSun $v[0] $v[1] $v[2]}catch{$failed=$true};Require $failed 'Incorrect actual day/night lighting accepted.'}
 Require ((Lamp-Live @('LAMP-001: 5 lamp simulations since start, 2 of them switched on, 1 lamp lights live in the scene')) -eq 1) 'Actual lamp scene-count parser failed.'
 Require ((Model-Stem '42: mc vojakw2') -ceq 'mc vojakw2') 'Actual shape debug-name parser failed.'
 Require ((Model-Stem '000000012345ABCD# NOID stan_eastc') -ceq 'stan_eastc') 'Actual local static EntityAI model name refused.'
 Require ((Model-Stem '0x1234abcd# 42: stan_eastc REMOTE') -ceq 'stan_eastc') 'Actual remote static EntityAI model name refused.'
 $failed=$false;try{Model-Stem 'WEST Alpha Schwarz:1 (mail)'}catch{$failed=$true};Require $failed 'Commander unit identity misread as static model shape.'
 foreach($v in @('42: <no shape>','objNull','')){$failed=$false;try{Model-Stem $v}catch{$failed=$true};Require $failed 'Missing actual model shape accepted.'}
 $views=Contact-Poses 10 20 30
 Require ($views.Count -eq 2 -and $views[0].pose.Count -eq 5 -and $views[1].pose.Count -eq 5) 'Contact camera tuple arity changed.'
 Require ($views[0].pose[0] -eq 8 -and $views[0].pose[1] -eq 20 -and $views[0].pose[2] -eq 30.6 -and
  $views[1].pose[0] -eq 10.2 -and $views[1].pose[1] -eq 20 -and $views[1].pose[2] -eq 38) 'Contact camera scalar offsets changed.'
 $e=@{texture='eden\ps.paa';quadrants=@(@{surfaceClass='Sand';files='ps??????';sound='sand';character=''})}
 $s=@{ok=$true;enabled=$true;world='eden.wrp';texture=$e.texture;surfaceClass='Sand';files='ps??????';sound='sand';character='';sourceEligible=$true;x=1;z=2;offset=0;surfaceY=10;surfaceDx=0;surfaceDz=0}
 Assert-Tuple $s $e 'eden' 'sand';$s.character='grass';$failed=$false;try{Assert-Tuple $s $e 'eden' 'sand'}catch{$failed=$true};Require $failed 'Conflicting clutter metadata admitted.'
 $s.character='';$s.surfaceClass='sand';Assert-Tuple $s $e 'eden' 'sand'
 $s.surfaceClass='Other';$failed=$false;try{Assert-Tuple $s $e 'eden' 'sand'}catch{$failed=$true};Require $failed 'Different class admitted as a casing alias.'
 $row='PLAYER_GROUND_BOOT time=2 manual=true side=left model=data3d\mc vojakw2.p3d primary=combat secondary=combat factor=1 x=4 y=5 z=6 ground=5 delta=0 road=5 surface=Sand files=ps?????? sound=sand mudActive=false sandActive=true mudChanged=false sandChanged=true'
 Require (@(Boot-Rows @($row) 'sand').Count -eq 1) 'Actual manual boot grammar not recognized.'
 Require (@(Boot-Rows @($row.Replace('manual=true','manual=false')) 'sand').Count -eq 0) 'AI surrogate accepted as manual proof.'
 $body=@('PLAYER_GROUND_QUEUE time=2 camera=2 forceLOD=-1 insideLOD=5 resident=true invisible=false coverage=0 model=data3d\mc vojakw2.p3d x=4 y=5 z=6',
  'PLAYER_GROUND_DRAW time=2 camera=2 level=0 inside=5 model=data3d\mc vojakw2.p3d x=4 y=5 z=6 ground=5 mud=0 sand=0 dead=false hidden=0.000',
  'PLAYER_GROUND_SECTION camera=2 hint=0 model=data3d\mc vojakw2.p3d texture=merged\00007mc_vojakw2.paa primaryMesh=true sections=0:1 indices=300 flags=0')
 $null=Assert-Body $body;$failed=$false;try{Assert-Body @($body|ForEach-Object{$_.Replace('primaryMesh=true','primaryMesh=false')})}catch{$failed=$true};Require $failed 'Weapon-only external draw accepted as body proof.'
 $failed=$false;try{Assert-Body $body 3}catch{$failed=$true};Require $failed 'Stale pre-weather body draw accepted as current visibility proof.'
 $null=Assert-Cloth @('UNIFORM_CLOTH model=data3d\mc vojakw2.p3d texture=merged\00007mc_vojakw2.paa kind=2 enabled=true wet=0.3000 encoded=2.3000')
 $failed=$false;try{Assert-Cloth @('UNIFORM_CLOTH model=data3d\mc vojakw2.p3d texture=merged\00007mc_vojakw2.paa kind=2 enabled=true wet=0.3000 encoded=0.3000')}catch{$failed=$true};Require $failed 'Wrong actual cloth upload accepted.'
 Write-Host 'Original-map tuple/manual-contact/cloth falsifiers PASS. No game/GPU/profile used.'
}
if($SelfTest){Test-Helpers;return}
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit.';Require ([bool]$env:LOCK_OWNER) 'Use scripts/with-game-lock.sh.'
Require ($Maps.Count-eq @($Maps|Select-Object -Unique).Count) 'Duplicate map arms would overwrite evidence.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve foreign game ownership.'
$before=Pair
if(!$SourceCensus){$SourceCensus=Join-Path $root 'docs/roadmap/modernisation/ORIGINAL-OFP-CONTACT-SOURCE-CENSUS-20261001.json'}
$census=Get-Content -LiteralPath $SourceCensus -Raw|ConvertFrom-Json
$output=Join-Path $root ('build/original-weather-player/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6));New-Item -ItemType Directory -Force -Path $output|Out-Null
$result=[ordered]@{status='running';sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;sourceCensusSha256=(Get-FileHash -LiteralPath $SourceCensus).Hash;installed=$before;lockOwner=$env:LOCK_OWNER;maps=@{};scope='Actual controlled SoldierWB, original maps, source-qualified contacts where available, real wet cloth and external-body source/captures. Unsupported soft sources explicit; appearance requires root inspection.'}
$result.helperSha256=@{};foreach($helper in @('Write-BenchmarkGraphics.ps1','Assert-CaptureLifecycle.ps1')){$result.helperSha256[$helper]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $helper)).Hash}
$result.sandSnapshotTrace=[bool]$SandSnapshotTrace
$result.uniformLightingViews=[bool]$UniformLightingViews
$result.terrainWetnessTrace=[bool]$TerrainWetnessTrace;$result.minimumMudWetness=$MinimumMudWetness
if($UniformLightingViews){. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1');$result.helperSha256['TerrainPuddleStockRoof.ps1']=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash}
if($SandSnapshotTrace){$result.helperSha256['Read-SandUploadSnapshot.ps1']=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Read-SandUploadSnapshot.ps1')).Hash}
$settings=@{POSEIDON_PLAYER_GROUND_TRACE='1';POSEIDON_UNIFORM_WET_TRACE='1';POSEIDON_SAND_TRACE='1';POSEIDON_MUD_TRACE='1';WGR_UNIFORM_WET='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_LOD_GOVERNOR_RANGE='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
if($TerrainWetnessTrace){$settings.WGR_TERRAIN_PUDDLE_FIXTURE='1';$result.terrainPuddlesInheritedControl=[Environment]::GetEnvironmentVariable('WGR_TERRAIN_PUDDLES','Process')}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_SAND_UPLOAD_TRACE','WGR_SAND_SNAPSHOT_FILE','WGR_SAND_SNAPSHOT_ROOT')
if($UniformLightingViews){$clear+=@('POSEIDON_HEADLAMP','POSEIDON_LAMP_LIGHTS','POSEIDON_LAMP_RADIUS_SCALE','POSEIDON_LAMP_BRIGHTNESS_SCALE','POSEIDON_LAMP_COLOR','POSEIDON_LAMP_AMBIENT_SCALE','POSEIDON_LAMP_CONE','POSEIDON_LAMP_SPILL','POSEIDON_LAMP_SPILL_RADIUS','POSEIDON_LAMP_SPILL_AMBIENT')}
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR');$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$reader=$null;$writer=$null;$held=$false;$armDir=$null;$log=$null;$deadline=$null
function Lines{
 if(!(Test-Path -LiteralPath $log)){return @()};$f=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$r=[IO.StreamReader]::new($f)
 try{$t=$r.ReadToEnd();$n=$t.LastIndexOf("`n");if($n -ge 0){return $t.Substring(0,$n).Split("`n")};return @()}finally{$r.Dispose()}
}
function Health{Require (!$p.HasExited) 'Owned game exited early.';Require ([DateTime]::UtcNow -lt $deadline) 'Map arm exceeded600s.';Require (!(Lines|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot')) 'Installed validation/script failure.'}
function Send($command){Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$writer.WriteLine($line)
 do{$line=$reader.ReadLine();Require ($null-ne $line) 'Harness closed.';$line|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null-eq $reply.ok);Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Field([string]$kind,$site){return Send @{cmd=('dev_'+$kind);action='sample';x=$site.x;z=$site.z}}
function State{$wet=Number (Eval 'triUniformWetness player');Require ($wet-ge 0 -and $wet-le 1) 'Actual Person wetness is unavailable/outside[0,1].';return @{time=(Number (Eval 'time'));position=(Eval 'getPosASL player');uniform=$wet;sand=(Send @{cmd='dev_sand';action='state'});mud=(Send @{cmd='dev_mud';action='state'});weather=(Send @{cmd='weather_visibility'})}}
function Terrain-WetnessReceipt($state){
 $rows=@(Terrain-WetnessRows (Lines));Require ($rows.Count -gt 0) 'Opt-in existing terrain weather diagnostic emitted no complete row.'
 return @{observationTime=$state.time;actualGMudWetness=$state.mud.wetness;actualLiquidRain=$state.weather.liquidRain;rowCount=$rows.Count;lastQueuedRow=$rows[-1];
  observedClockMinusLoggedClock=$state.time-$rows[-1].time;
  scope='Existing C++ presentation accumulator and queued pad1 only, logged every five simulation seconds. GMud geometry wetness is independent. No Rust acceptance, GPU completion, current-frame identity, local shader rain coverage or visible coat witness.'}
}
function Native-Heights($site){
 $initial=State;$tileX=[Math]::Floor($site.x/200);$tileZ=[Math]::Floor($site.z/200)
 # Coarse parents are centred at25m; production tile boundaries start half
 # a coarse cell earlier, at native index32*tile-2.
 $originX=$tileX*200-12.5;$originZ=$tileZ*200-12.5
 $header=Send @{cmd='dev_terrain_brush';action='state';x=$originX;z=$originZ}
 Require ($header.readonly -eq $true -and $header.baseline -eq $false -and $header.spacing -eq 6.25 -and $header.range -eq 2048) 'Native fixture requires an unedited actual2048/6.25 source.'
 Require ($originX -gt 0 -and $originZ -gt 0 -and $originX+200 -lt ($header.range-1)*6.25 -and $originZ+200 -lt ($header.range-1)*6.25) 'Native fixture tile is not complete interior source.'
 $vertices=@();$until=[DateTime]::UtcNow.AddSeconds(180)
 for($z=0;$z -le 32;++$z){for($x=0;$x -le 32;++$x){
  Require ([DateTime]::UtcNow -lt $until) 'Read-only native fixture exceeded bounded180s.'
  $px=$originX+$x*6.25;$pz=$originZ+$z*6.25;$v=Send @{cmd='dev_terrain_brush';action='state';x=$px;z=$pz}
  Require ($v.readonly -eq $true -and $v.baseline -eq $false -and $v.range -eq $header.range -and $v.spacing -eq $header.spacing -and $v.heightRevision -eq $header.heightRevision -and $v.seaLevel -eq $header.seaLevel) 'Actual native source identity changed during paused reads.'
  Require ($v.vertexWorldX -eq $px -and $v.vertexWorldZ -eq $pz -and $v.vertexX -eq $px/6.25 -and $v.vertexZ -eq $pz/6.25) 'Native nearest-vertex query differs from exact requested coordinate.'
  $h=Number $v.vertexHeight;$vertices+=,@($px,$pz,$h)
 }}
 $final=State;Assert-Paused $initial $final
 $fixture=@{scope='Actual unedited installed native vertices; no resampling, edit, water seed or offline detail reconstruction';map=$map;source=$source;installed=$before;header=$header;origin=@($originX,$originZ);tile=@($tileX,$tileZ);originConvention='200*tile-12.5; native index32*tile-2';side=33;order='z-major then x';vertices=$vertices;before=$initial;after=$final}
 $path=Join-Path $armDir 'actual-native-height-tile.json';$fixture|ConvertTo-Json -Depth 15|Set-Content -LiteralPath $path
 return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;count=$vertices.Count;header=$header;origin=$fixture.origin}
}
function Assert-Paused($a,$b){Require ($a.time -eq $b.time -and $a.uniform -eq $b.uniform) 'Paused clock/Person wetness changed.'
 foreach($kind in @('sand','mud')){foreach($f in @('enabled','wetness','chunks','revision','rejected')){Require ($a.$kind.$f -eq $b.$kind.$f) 'Paused physical store changed.'}}
 for($i=0;$i -lt 3;++$i){Require ($a.position[$i] -eq $b.position[$i]) 'Paused actual player moved.'}
 foreach($f in @('rain','liquidRain','particleDensity','particleSnowflakes')){Require ($a.weather.$f -eq $b.weather.$f) 'Paused real weather changed.'}
}
function Capture([string]$name){
 Start-Sleep -Milliseconds 500;$path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
 do{Health;$bytes=$null;if(Test-Path -LiteralPath $path){try{$bytes=[IO.File]::ReadAllBytes($path)}catch [IO.IOException]{}}
  if($bytes -and $bytes.Length -gt 36 -and ($bytes[($bytes.Length-8)..($bytes.Length-5)]-join ',') -eq '73,69,78,68'){break};Require ([DateTime]::UtcNow-lt $until) 'Complete PNG absent.';Start-Sleep -Milliseconds 100
 }while($true)
 Require ([BitConverter]::ToString($bytes[0..7])-ceq '89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG.'
 Require (($bytes[16..19]-join ',') -eq '0,0,5,0' -and ($bytes[20..23]-join ',') -eq '0,0,2,208') 'PNG not1280x720.'
 $arm.captures[$name]=@{path=$path;bytes=$bytes.Length;sha256=(Get-FileHash -LiteralPath $path).Hash;state=(State)}
 if($SandSnapshotTrace -and $name.StartsWith('sand-')){
  . (Join-Path $PSScriptRoot 'Read-SandUploadSnapshot.ps1')
  $captureFile=Join-Path $armDir ($name+'.sand.f32')
  $snapshotDeadline=[DateTime]::UtcNow.AddSeconds(10)
  do {
   Health;$snapshot=$null
   try {
    $snapshot=Read-SandUploadSnapshot $sandSnapshotFile
    $queued=@(Lines|Where-Object{$_ -match ('SAND_UPLOAD_QUEUED .*hash='+$snapshot.hash+'\r?$')}|Select-Object -Last 1)
    $dumped=@(Lines|Where-Object{$_ -match ('SAND_UPLOAD_SNAPSHOT written=true .*hash='+$snapshot.hash+' file=')}|Select-Object -Last 1)
    $received=@(Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue|Where-Object{$_ -match ('SAND_UPLOAD_RECEIVED accepted=true .*hash='+$snapshot.hash+' ')}|Select-Object -Last 1)
    if($queued.Count -and $dumped.Count -and $received.Count){
     Copy-Item -LiteralPath $sandSnapshotFile -Destination $captureFile
     $copied=Read-SandUploadSnapshot $captureFile
     Require ($copied.hash -ceq $snapshot.hash -and $copied.sha256 -ceq $snapshot.sha256) 'Sand payload changed during capture copy.'
     $snapshot.publication=Assert-SandUploadPublication $copied $queued[0] $received[0]
     $snapshot.path=$captureFile;$snapshot.queued=$queued[0];$snapshot.dumped=$dumped[0];$snapshot.received=$received[0]
     $arm.captures[$name].sandSnapshot=$snapshot;break
    }
   } catch [IO.IOException] {}
   Require ([DateTime]::UtcNow-lt $snapshotDeadline) 'Actual sand snapshot/dump/accepted renderer receipt mismatch or missing.'
   Start-Sleep -Milliseconds 100
  }while($true)
 }
}
function Camera([string]$mode){Exec ('player switchCamera "'+$mode+'"');Start-Sleep -Milliseconds 300;Require ((Number (Eval 'triGetCameraEffectActive')) -eq 0) 'Camera effect suspends controlled input.'}
function Return-Views([string]$name){$a=State;Camera 'INTERNAL';Capture ($name+'-first');Camera 'EXTERNAL';Capture ($name+'-third-return');$b=State;Assert-Paused $a $b;return @{before=$a;after=$b}}
function Place($site){
 Exec ('setAccTime 0;player setPos ['+$site.x+','+$site.z+',0];player setDir 90;player setUnitPos "UP"');Camera 'EXTERNAL'
 Exec 'setAccTime 1';Start-Sleep -Seconds 1;Exec 'setAccTime 0'
 Require ((Eval 'typeOf player')-ceq 'SoldierWB' -and (Eval 'alive player')-eq $true) 'Actual controlled standard US actor differs/died.'
 return State
}
function Source-Lane($site,[string]$kind){
 $api=if($kind-eq 'uniform'){'mud'}else{$kind}
 $samples=@();foreach($dz in @(-8,0,8)){foreach($dx in @(-8,0,8)){$s=Field $api @{x=($site.x+$dx);z=($site.z+$dz)};Assert-Tuple $s $site $map $kind;$samples+=$s}}
 # Actual all-four source quadrants, not just a representative centre label.
 $cx=[Math]::Floor($site.x/50);$cz=[Math]::Floor($site.z/50)
 foreach($v in @(.25,.75)){foreach($u in @(.25,.75)){$s=Field $api @{x=(($cx+$u)*50);z=(($cz+$v)*50)};Assert-Tuple $s $site $map $kind;$samples+=$s}}
 return $samples
}
function Walk([string]$name,$site,[string]$kind,[bool]$positive){
 $start=Eval 'getPosASL player';$lineStart=@(Lines).Count;$before=State
 Require ((Number (Eval 'triGetCameraEffectActive'))-eq 0) 'Ordinary player controls suspended.'
 $input=Eval 'triGetInputContext';Exec 'setAccTime 1';$null=Send @{cmd='key';sc=26;hold=$true};$script:held=$true;$until=[DateTime]::UtcNow.AddSeconds(25)
 do{Start-Sleep -Milliseconds 150;$pos=Eval 'getPosASL player';$distance=[Math]::Sqrt([Math]::Pow(([double]$pos[0]-[double]$start[0]),2)+[Math]::Pow(([double]$pos[1]-[double]$start[1]),2));Require ([DateTime]::UtcNow-lt $until) 'Ordinary SDL movement did not travel4m.';Require ([Math]::Abs($pos[0]-$site.x) -lt 8 -and [Math]::Abs($pos[1]-$site.z) -lt 8) 'Actual player left the source-proved lane.'}while($distance-lt 4)
 $null=Send @{cmd='key_up';sc=26};$script:held=$false;Start-Sleep -Milliseconds 300;Exec 'setAccTime 0';$after=State
 Require ((Eval 'alive player')-eq $true) 'Actual player died.'
 $lines=@(Lines);$fresh=@($lines|Select-Object -Skip $lineStart);$boots=@(Boot-Rows $fresh $kind);$samples=@()
 if($positive){Require ($boots.Count-gt 0 -and $after.$kind.revision-gt $before.$kind.revision) 'Living ordinary PlayerOn made no changed physical contacts.'
  foreach($boot in @($boots|Select-Object -Last 6)){foreach($dx in @(-.125,0,.125)){$s=Field $kind @{x=($boot.x+$dx);z=$boot.z};Assert-Tuple $s $site $map $kind;$samples+=$s}}
  Require (@($samples|Where-Object{(Number $_.offset)-lt -.001}).Count-gt 0) 'Actual changed boot route produced no sampled signed depression.'
 }else{Require ($boots.Count-eq 0) 'Dry/unqualified soil generated mud contact.'}
 $record=@{start=$start;end=$after.position;distance=$distance;inputContext=$input;input='Ordinary SDL W; no doMove/selectPlayer/forced animation/stamp';before=$before;after=$after;actualChangedBootRows=$boots;samples=$samples;body=(Assert-Body (Lines) ($after.time-.65))}
 Capture $name;$record.return=Return-Views $name
 if($samples.Count){$returned=@();foreach($s in $samples){$n=Field $kind $s;Require ($n.offset-eq $s.offset -and $n.surfaceY-eq $s.surfaceY -and $n.surfaceDx-eq $s.surfaceDx -and $n.surfaceDz-eq $s.surfaceDz) 'Camera return changed physical geometry samples.';$returned+=$n};$record.returnedSamples=$returned}
 return $record
}
function Contact-Views($record,[string]$kind){
 Require ($record.samples.Count -gt 0) 'No actual walked contact samples to inspect.'
 $walked=$record.samples|Sort-Object offset|Select-Object -First 1
 # Rain may legitimately evolve a previously walked field before this final
 # paused inspection. Compare camera queries to the current actual baseline.
 $sample=Field $kind $walked;Require ($sample.offset -lt -.001) 'Walked cell no longer retains signed contact relief.'
 $initial=State;$x=[double]$sample.x;$z=[double]$sample.z;$height=[double]$sample.surfaceY
 foreach($view in (Contact-Poses $x $z $height)){
  $pose=($view.pose|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ' '
  Require ((Eval ('triFreeFlyPose "'+$pose+'"')) -ceq 'OK') 'Contact inspection freefly refused.'
  Capture ($kind+'-actual-contact-'+$view.name)
  $actual=Field $kind $sample
  foreach($field in @('offset','surfaceY','surfaceDx','surfaceDz')){Require ($actual.$field -eq $sample.$field) 'Inspection changed actual walked contact geometry.'}
  Assert-Paused $initial (State)
 }
 $resultViews=@{sample=$sample;scope='Actual previously walked cell, paused low/top inspection; freefly uses existing camera focus, not actor focus'}
 if($TerrainWetnessTrace -and $kind -ceq 'mud'){$resultViews.terrainWetness=Terrain-WetnessReceipt (State)}
 return $resultViews
}
function Terrain-MudCentreViews($site){
 $sample=Field 'mud' $site;Assert-Tuple $sample $site $map 'mud';$initial=State
 foreach($view in (Contact-Poses ([double]$sample.x) ([double]$sample.z) ([double]$sample.surfaceY))){
  $pose=($view.pose|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ' '
  Require ((Eval ('triFreeFlyPose "'+$pose+'"')) -ceq 'OK') 'Actual admitted mud centre inspection refused.'
  Capture ('mud-source-centre-'+$view.name);Assert-Paused $initial (State)
  $after=Field 'mud' $sample
  foreach($field in @('offset','surfaceY','surfaceDx','surfaceDz')){Require ($after.$field -eq $sample.$field) 'Centre inspection changed actual source ground geometry.'}
 }
 return @{sample=$sample;terrainWetness=(Terrain-WetnessReceipt (State));scope='Actual admitted mud source centre and physical height, paused low/top images. Centre may be untouched; walked contact views identify actual compressed cells separately.'}
}
function Uniform-Lighting([string]$band){
 $original=State
 Require ((Eval 'typeOf player') -ceq 'SoldierWB' -and (Eval 'alive player') -eq $true) 'Uniform fixture lost the actual stock soldier.'
 $direction=Number (Eval 'getDir player')
 if($band -eq 'dry'){
  Require ($original.uniform -lt .001) 'Dry uniform comparison is already wet.'
  $arm.uniformViewAnchor=@($original.position);$arm.uniformViewDirection=$direction
 }
 else{Require ($original.uniform -ge .3) 'Wet uniform comparison lacks actual rain history.'}
 Assert-UniformHeading $direction $arm.uniformViewDirection
 $anchor=@($arm.uniformViewAnchor);$view=Uniform-View $anchor $arm.uniformViewDirection
 for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $original.position[$i])-(Number $anchor[$i])) -le .01) 'Dry/wet actor left the same standing comparison position.'}
 $actor=Eval 'format ["%1",player]';$body=Assert-Body (Lines) ($original.time-.65)
 Require ($actor -is [string] -and ![string]::IsNullOrWhiteSpace($actor) -and $actor -cne '<nullptr-object>') 'Formatted actual controlled player identity is absent.'
 if($band -eq 'dry'){$arm.uniformActorDebug=$actor}else{Require ($actor -ceq $arm.uniformActorDebug) 'Dry/wet actual actor differs.'}
 $record=@{before=$original;actor=@{class=(Eval 'typeOf player');debugName=$actor;model='data3d\mc vojakw2.p3d';modelSource=$body;position=$original.position;direction=$direction;anchoredDryDirection=$arm.uniformViewDirection;initialPlacementDirectionRequest=90};camera=@{rawEngineView=$view;derivedFromSettledDryDirection=$arm.uniformViewDirection;command='triSetView';effectActive=(Eval 'triGetCameraEffectActive')};views=@{};scope='Same stock standing actor, anchored actual settled dry heading and render-only camera, matching actual overcast. Rain differs between natural wetting histories. Ordinary idle animation may differ; no pose/bone equivalence or pixelwise equality claim.'}
 $lampOwned=$false;$roofOwned=$false
 try{
  # World::SetWeather applies zero-duration overcast immediately. Unlike rain,
  # changing sky/cloud lighting needs no simulation step. Rain history stays real.
  Exec '0 setOvercast 1;setDate [1985,6,21,16,0]';$baseline=State;Assert-Paused $original $baseline
  Require ((Number $baseline.weather.overcast) -ge .99) 'Paused matching stock cloud lighting did not apply.'
  $pose=($view|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ','
  Camera 'EXTERNAL';Require ((Eval ('triSetView ['+$pose+']')) -ceq 'OK') 'Uniform render-only view refused.'
  Require ((Number (Eval 'triGetCameraEffectActive')) -eq 0) 'Uniform render-only view suspended player controls.'
  $lampX=(Number $anchor[0])+1.5;$lampZ=(Number $anchor[1])+.7
  $ground=Get-TerrainPuddleFixtureHeight ${function:Send} $lampX $lampZ
  $lampPosition=@($lampX,$lampZ,$ground);$tuple=($lampPosition|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ','
  Exec ('uniformStockLamp="StreetLampWood" createVehicle ['+$tuple+'];uniformStockLamp setPosASL ['+$tuple+'];uniformStockLamp allowDamage false;uniformStockLamp switchLight "OFF"');$lampOwned=$true
  Require ((Eval 'typeOf uniformStockLamp') -ceq 'StreetLampWood' -and (Eval 'isNull uniformStockLamp') -eq $false) 'Real stock lamp failed to spawn.'
  $lampDebug=Eval 'format ["%1",uniformStockLamp]';$lampModel=Model-Stem $lampDebug;$actualLamp=Eval 'getPosASL uniformStockLamp'
  for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $actualLamp[$i])-$lampPosition[$i]) -le .01) 'Stock lamp placement differs from the matching fixture.'}
  if($band -eq 'dry'){$arm.uniformLampModel=$lampModel}else{Require ($lampModel -ceq $arm.uniformLampModel) 'Dry/wet stock lamp model differs.'}
  $record.lamp=@{class=(Eval 'typeOf uniformStockLamp');debugName=$lampDebug;modelStem=$lampModel;position=$actualLamp;requestedPosition=$lampPosition;config='Installed stock class and light parameters; no lamp data overrides'}
  $snapshot={param([string]$name,[bool]$night,[string]$lampState)
   $hour=Number (Eval 'dayTime');$sun=Number (Eval 'triGetShadowSunFactor');Assert-UniformSun $hour $sun $night
   $stateBefore=State;Assert-Paused $baseline $stateBefore
   $headingBefore=Number (Eval 'getDir player');Assert-UniformHeading $headingBefore $arm.uniformViewDirection
   Require ((Eval 'typeOf uniformStockLamp') -ceq 'StreetLampWood') 'Actual lamp identity disappeared before capture.'
   $lampNow=Eval 'getPosASL uniformStockLamp';for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $lampNow[$i])-$lampPosition[$i]) -le .01) 'Actual lamp moved between lighting captures.'}
   Require ((Number (Eval 'triGetCameraEffectActive')) -eq 0) 'Paused render-only camera became a gameplay camera.'
   Capture ('uniform-'+$band+'-'+$name);$stateAfter=State;Assert-Paused $stateBefore $stateAfter
   $headingAfter=Number (Eval 'getDir player');Assert-UniformHeading $headingAfter $headingBefore;Assert-UniformHeading $headingAfter $arm.uniformViewDirection
   Require ((Number (Eval 'dayTime')) -eq $hour -and (Number (Eval 'triGetShadowSunFactor')) -eq $sun) 'Actual lighting changed during paused capture.'
   $record.views[$name]=@{before=$stateBefore;after=$stateAfter;headingBefore=$headingBefore;headingAfter=$headingAfter;hour=$hour;shadowSunFactor=$sun;lampStateCommand=$lampState;lampLive=(Lamp-Live (Lines));lampSourceRows=@(Lines|Where-Object{$_ -match 'LAMP-001:|LAMP-003 type StreetLampWood:'}|Select-Object -Last 3);lampClass=(Eval 'typeOf uniformStockLamp');lampPosition=$lampNow;cameraEffectActive=(Eval 'triGetCameraEffectActive');body=(Assert-Body (Lines) ($baseline.time-.65))}
  }
  & $snapshot 'day-close' $false 'OFF'
  Exec 'setDate [1985,6,21,0,0];uniformStockLamp switchLight "OFF"';Start-Sleep -Milliseconds 500
  & $snapshot 'night-lamp-off' $true 'OFF';$off=[int]$record.views['night-lamp-off'].lampLive
  Exec 'uniformStockLamp switchLight "ON"';Start-Sleep -Milliseconds 500
  & $snapshot 'night-lamp-on' $true 'ON';Require ($record.views['night-lamp-on'].lampLive -eq $off+1) 'Real stock lamp ON added no exact one scene light.'
  Exec 'uniformStockLamp switchLight "OFF"';Start-Sleep -Milliseconds 500
  & $snapshot 'night-lamp-off-return' $true 'OFF';Require ($record.views['night-lamp-off-return'].lampLive -eq $off) 'Real lamp OFF did not remove its scene light.'
  Exec 'setDate [1985,6,21,16,0]';Start-Sleep -Milliseconds 500
  & $snapshot 'roof-absent' $false 'OFF'
  $roof=New-TerrainPuddleStockRoof -SendCommand ${function:Send} -X $anchor[0] -Z $anchor[1] -RoofLift 3;$roofOwned=$true
  Require ($roof.class -ceq 'CampEastC' -and $roof.model -ceq 'stan_eastC.p3d' -and $roof.roofLift -eq 3) 'Lifted stock roof identity differs.'
  & $snapshot 'roof-present' $false 'OFF'
  $record.views['roof-present'].roof=@{class=(Eval 'typeOf puddleStockRoof');debugName=(Eval 'format ["%1",puddleStockRoof]');position=(Eval 'getPosASL puddleStockRoof');fixture=$roof}
  Require ($record.views['roof-present'].roof.class -ceq 'CampEastC' -and (Model-Stem $record.views['roof-present'].roof.debugName) -ceq 'stan_eastc') 'Captured roof model/class differs from its actual master.'
  for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $record.views['roof-present'].roof.position[$i])-(Number $roof.actualPosition[$i])) -le .01) 'Captured roof master moved.'}
  Remove-TerrainPuddleStockRoof -SendCommand ${function:Send};$roofOwned=$false
  & $snapshot 'roof-removed' $false 'OFF';$record.views['roof-removed'].actualRoofRemoved=Eval 'isNull puddleStockRoof'
  Require ($record.views['roof-removed'].actualRoofRemoved -eq $true) 'Actual roof master removal absent.'
  $record.pausedRoofRetention=@{before=$record.views['roof-absent'].before.uniform;covered=$record.views['roof-present'].after.uniform;after=$record.views['roof-removed'].after.uniform;scope='Paused retention/lighting only; does not prove shelter stops wetting during live rain'}
 }finally{
  if($roofOwned){Remove-TerrainPuddleStockRoof -SendCommand ${function:Send}}
  if($lampOwned){Exec 'uniformStockLamp switchLight "OFF";deleteVehicle uniformStockLamp';Require ((Eval 'isNull uniformStockLamp') -eq $true) 'Owned comparison lamp deletion failed.'}
  Require ((Eval 'triClearView') -ceq 'OK') 'Uniform render-only camera did not release.'
  Exec ('setDate [1985,6,21,16,0];0 setOvercast '+([double]$original.weather.overcast).ToString('R',$culture))
 }
 Start-Sleep -Milliseconds 500;$record.after=State;Assert-Paused $original $record.after
 Require ([Math]::Abs((Number $record.after.weather.overcast)-(Number $original.weather.overcast)) -lt .001) 'Original cloud lighting was not restored.'
 Assert-UniformHeading (Number (Eval 'getDir player')) $direction
 return $record
}
try{
 foreach($key in $clear){Restore-Env $key $null};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 foreach($map in $Maps){
  Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.';Same-Pair $before (Pair)
  $source=$census.maps.$map;Require ($null-ne $source) 'Selected map absent from source census.'
  $asset=if($map-eq 'noe'){Join-Path $GameDir 'AddOns/Noe.pbo'}else{Join-Path $GameDir ('Worlds/'+$map+'.wrp')}
  $expected=if($map-eq 'noe'){$census.noeArchiveSha256}else{$source.wrpSha256};Require ((Get-FileHash -LiteralPath $asset).Hash.Equals($expected,[StringComparison]::OrdinalIgnoreCase)) 'Actual original map corpus changed; regenerate source census.'
  $armDir=Join-Path $output $map;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force -Path $profile|Out-Null
  & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
  $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;').Replace('fpsCap=0;','fpsCap=60;')))
  $mission=Join-Path $armDir ('controlled.'+$map);New-Item -ItemType Directory -Force -Path $mission|Out-Null
  Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
  $sand=@($source.controls.sand.candidates)|Select-Object -First 1;if($map-eq 'noe' -and $source.preferredSand){$sand=$source.preferredSand}
  $soil=@($source.controls.mud.candidates)|Select-Object -First 1;$uniform=if($soil){$soil}elseif($sand){$sand}else{@($source.controls.uniform.candidates)|Select-Object -First 1}
  Require ($null-ne $uniform) 'No source dry-land candidate for actual cloth/body fixture.'
  $arm=@{status='running';map=$map;source=$source;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;profileSha256=(Get-FileHash -LiteralPath $cfg).Hash;softCoverage=@{sand=([bool]$sand);mud=([bool]$soil);unsupported='Missing admitted families remain unsupported, never inferred as soft from Default/brown appearance'};captures=@{};gates=@{}};$result.maps[$map]=$arm
  $env:POSEIDON_USER_DIR=$profile;$log=Join-Path $armDir 'engine.log';$stdout=Join-Path $armDir 'stdout.txt';$stderr=Join-Path $armDir 'stderr.txt'
  $sandSnapshotFile=Join-Path $armDir 'latest-sand-snapshot.f32'
  if($SandSnapshotTrace){$env:WGR_SAND_UPLOAD_TRACE='1';$env:WGR_SAND_SNAPSHOT_FILE=$sandSnapshotFile;$env:WGR_SAND_SNAPSHOT_ROOT=$armDir}
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
  $deadline=[DateTime]::UtcNow.AddSeconds(600);$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr;$null=$p.Handle;$arm.pid=$p.Id
  $until=[DateTime]::UtcNow.AddSeconds(120);do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Harness startup timeout.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady')-ceq 'OK') 'Original world mission not ready.'
  Exec 'player allowDamage false;0 setFog 0;0 setOvercast 0;0 setRain 0;setDate [1985,6,21,16,0];setAccTime 0'
  $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
  if($NativeHeightFixture){$arm.gates.nativeHeightFixture=Native-Heights $uniform}
  if($sand){$null=Place $sand;$arm.gates.sandSource=Source-Lane $sand 'sand';$arm.gates.sandWalk=Walk 'sand-dry-third' $sand 'sand' $true}
  $null=Place $uniform;$arm.gates.uniformSource=Source-Lane $uniform 'uniform'
  # Uniform/body tests need real terrain support, not mud eligibility.
  if($soil){$arm.gates.drySoilWalk=Walk 'soil-dry-third' $soil 'mud' $false;$null=Place $soil}
  elseif(!$sand){$arm.gates.unqualifiedDryWalk=Walk 'unqualified-dry-third' $uniform 'mud' $false;$null=Place $uniform}
  $dry=State;Require ($dry.uniform-lt .001 -and $dry.weather.liquidRain-lt .001) 'Dry clothing/weather control not initially dry.';Capture 'uniform-dry-third';$arm.gates.dry=$dry
  if($UniformLightingViews){$arm.gates.uniformLightingDry=Uniform-Lighting 'dry'}
  $arm.gates.wetWait=@{requestedMinimumMudWetness=$MinimumMudWetness;hasAdmittedMudSource=[bool]$soil;wallDeadlineSeconds=$WetDeadlineSeconds;start=State}
  Exec '0 setOvercast 1;0 setRain 1;setAccTime 4';$until=[DateTime]::UtcNow.AddSeconds($WetDeadlineSeconds)
  do{Start-Sleep -Milliseconds 400;$wet=State;$arm.gates.wetWait.lastActual=$wet;Require ([DateTime]::UtcNow-lt $until) 'Natural liquid rain did not reach actual cloth/GMud thresholds within the bounded wetting deadline.'}while(!(Mud-WetReady $wet ([bool]$soil) $MinimumMudWetness))
  Exec 'setAccTime 0';$wet=State;Require (!$wet.weather.particleSnowflakes -and $wet.weather.particleDensity-eq 0) 'Wetting uses snow/particle override instead of actual liquid rain.'
  $arm.gates.wet=$wet;$arm.gates.cloth=Assert-Cloth (Lines);$arm.gates.wetBody=Assert-Body (Lines) ($wet.time-.65);Capture 'uniform-wet-third';$arm.gates.wetReturn=Return-Views 'uniform-wet'
  if($TerrainWetnessTrace){$arm.gates.terrainWetReady=Terrain-WetnessReceipt $wet}
  if($UniformLightingViews){$arm.gates.uniformLightingWet=Uniform-Lighting 'wet'}
  if($soil){$arm.gates.wetSoilWalk=Walk 'mud-wet-third' $soil 'mud' $true}
  else{$arm.gates.unqualifiedWetWalk=Walk 'unqualified-mud-wet-third' $uniform 'mud' $false}
  $a=State;Start-Sleep -Seconds 2;$b=State;Assert-Paused $a $b;$arm.gates.paused=@{before=$a;after=$b}
  if($ContactViews){
   if($sand){$arm.gates.sandCloseViews=Contact-Views $arm.gates.sandWalk 'sand'}
   if($soil){$arm.gates.mudCloseViews=Contact-Views $arm.gates.wetSoilWalk 'mud'}
  }
  if($TerrainWetnessTrace -and $soil){
   if(!$ContactViews){$arm.gates.mudCloseViews=Contact-Views $arm.gates.wetSoilWalk 'mud'}
   $arm.gates.mudCentreViews=Terrain-MudCentreViews $soil
  }
  Exec 'setAccTime 1';$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and $p.ExitCode-eq 0) 'Owned normal exit failed.'
  Require ((Lines|Select-String 'Shutdown complete')-and !(Lines|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION')) 'Exit/validation proof missing.'
  $arm.lifecycle=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$meta=Join-Path $armDir 'lifecycle.json';$arm.lifecycle|ConvertTo-Json|Set-Content -LiteralPath $meta
  & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $meta -LogPath $log
  Same-Pair $before (Pair);$arm.status='functional-available-sources-passed-body-appearance-pending';$client.Dispose();$client=$null;$p=$null
 }
 $result.status='functional-available-sources-passed-explicit-soft-gaps-visual-review-pending'
}catch{$result.status='failed';$result.error=$_.Exception.Message;throw}
finally{
 if($p -and !$p.HasExited -and $client){try{if($held){$null=Send @{cmd='key_up';sc=26};$held=$false};$writer.WriteLine('{"cmd":"exit"}');$null=$reader.ReadLine();$null=$p.WaitForExit(20000)}catch{$result.cleanupError=$_.Exception.Message}}
 if($client){$client.Dispose()};if($p -and !$p.HasExited){$result.status='failed';$result.ownedForcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$null=$p.WaitForExit(10000)}
 foreach($key in $keys){Restore-Env $key $saved[$key]};$result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Original player record: $output"
}
