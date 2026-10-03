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
 [switch]$MudLightingViews,
 [switch]$WetSoilFragmentViews,
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
function Mud-LightingWetReady($state,$receipt){
 if(!(Mud-WetReady $state $true .8)){return $false}
 $row=$receipt.lastQueuedRow
 foreach($field in @('accumulated','queuedPad1')){$n=Number $row.$field;Require ($n -ge 0 -and $n -le 1) 'Invalid existing queued wet-soil diagnostic.'}
 # The existing log rounds to four decimals. Reserve half a printed unit,
 # rather than admitting a real .79996 as >=.8 because it prints .8000.
 return $row.accumulated -ge .8001 -and $row.queuedPad1 -ge .8001 -and
  [Math]::Abs($row.accumulated-$row.queuedPad1) -le .00011
}
function Mud-LightingPlan{return @(@{name='overcast';overcast=1},@{name='clear';overcast=0})}
function Wet-SoilModes{return @(
 @{mode=1;name='source';rgb=@('source soil fraction','initial soil wet amount before shelter','queued terrain wetness')},
 @{mode=2;name='rain';rgb=@('local coverage','local vertical reach','actual rain proof; zeros if production did not query')},
 @{mode=3;name='coat';rgb=@('final admitted wet soil','remaining cosmetic film','procedural film before shelter/partition')},
 @{mode=4;name='water';rgb=@('neutral terrain; magenta actual surviving retained water','actual water alpha preserved','postprocess prevents numeric channel decoding')})
}
function Assert-DiagnosticCamera($a,$b){
 foreach($field in @('position','direction','up')){
  Require (@($a.$field).Count -eq 3 -and @($b.$field).Count -eq 3) 'Actual diagnostic camera vectors absent.'
  for($i=0;$i -lt 3;++$i){Require ((Number $a.$field[$i]) -eq (Number $b.$field[$i])) 'Actual diagnostic scene camera changed between modes.'}
 }
}
function Quality-CloudPin([bool]$quality){if(!$quality){return '0'};return $null}
function Assert-RendererClouds($weather,[double]$overcast){
 Require ($weather.rendererSkyAvailable -is [bool] -and $weather.rendererSkyAvailable) 'Actual producer sky settings are unavailable.'
 $sky=$weather.rendererSky
 foreach($field in @('enabled','skyLighting','coverageFromWeather','coveragePinned')){Require ($sky.$field -is [bool]) 'Malformed producer sky setting.'}
 Require ($sky.enabled -and $sky.skyLighting -and $sky.coverageFromWeather -and !$sky.coveragePinned) 'Quality sky must be enabled, weather-driven and unpinned.'
 foreach($field in @('coverage','clearCoverage','fullCoverage')){$n=Number $sky.$field;Require ($n -ge 0 -and $n -le 1) 'Actual producer cloud coverage outside[0,1].'}
 Require ($sky.fullCoverage -gt $sky.clearCoverage) 'Actual sky has no distinct clear/overcast coverage.'
 $actual=Number $weather.overcast;Require ([Math]::Abs($actual-$overcast) -le .001) 'Actual world cloud command differs.'
 $expected=$sky.clearCoverage+($sky.fullCoverage-$sky.clearCoverage)*$actual
 Require ([Math]::Abs($sky.coverage-$expected) -le .001) 'Latest producer cloud setting does not yet follow actual world overcast.'
}
function Renderer-CloudRecipe($weather){
 $sky=$weather.rendererSky
 return [ordered]@{enabled=$sky.enabled;skyLighting=$sky.skyLighting;coverageFromWeather=$sky.coverageFromWeather;coveragePinned=$sky.coveragePinned;clearCoverage=$sky.clearCoverage;fullCoverage=$sky.fullCoverage}
}
function Assert-CloudRecipe($a,$b){Require (($a|ConvertTo-Json -Compress) -ceq ($b|ConvertTo-Json -Compress)) 'Producer cloud recipe changed between quality views.'}
function Assert-MudSun($weather,[double]$hour,[double]$overcast){
 $null=Number $hour
 Require ([Math]::Abs($hour-16) -lt .001 -and $weather.sunAvailable -is [bool] -and $weather.sunAvailable) 'Actual 16h mud daylight is absent.'
 foreach($field in @('sunTravelY','overcast','nightEffect','solarDrying')){$null=Number $weather.$field}
 Require ($weather.sunTravelY -lt 0 -and $weather.sunTravelY -ge -1 -and $weather.nightEffect -ge 0 -and $weather.nightEffect -le .001 -and [Math]::Abs($weather.overcast-$overcast) -le .001) 'Actual above-horizon sun/cloud/night contract differs.'
 $expected=(-$weather.sunTravelY)*(1-$weather.overcast)*(1-$weather.overcast)*(1-$weather.nightEffect)
 Require ([Math]::Abs($weather.solarDrying-$expected) -le .00001) 'Actual solar exposure differs from shared sun-travel/cloud policy.'
 if($overcast -eq 0){Require ($weather.solarDrying -gt .01) 'Clear mud comparison has no actual daylight exposure.'}
 else{Require ($weather.solarDrying -le .00001) 'Overcast mud comparison has unexpected direct solar exposure.'}
}
function Mud-RestoreDateTuple([double]$hour){
 $null=Number $hour
 # This runner owns a fixed 1985-06-21 afternoon mission. No date getter exists.
 Require ($hour -ge 16 -and $hour -lt 17) 'Actual mud fixture left its owned same-date afternoon interval.'
 $h=[int][Math]::Floor($hour);$m=[int][Math]::Floor(($hour-$h)*60)
 return @(1985,6,21,$h,$m)
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
function Uniform-StageCommand([double]$heading){
 $null=Uniform-HeadingDifference $heading $heading
 return 'player setDir '+$heading.ToString('R',$culture)
}
function Assert-UniformSources($a,$b){
 foreach($kind in @('sand','mud')){
  Require (($a[$kind]|ConvertTo-Json -Depth 8 -Compress) -ceq ($b[$kind]|ConvertTo-Json -Depth 8 -Compress)) 'Paused uniform orientation staging changed actual source/geometry.'
 }
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
 $modes=@(Wet-SoilModes);Require (($modes.mode -join ',') -ceq '1,2,3,4' -and $modes.Count -eq 4) 'Bounded actual fragment diagnostic recipe changed.'
 $cam=@{position=@(1,2,3);direction=@(1,0,0);up=@(0,1,0)};Assert-DiagnosticCamera $cam $cam
 $other=$cam|ConvertTo-Json|ConvertFrom-Json;$other.position[0]=4;$failed=$false;try{Assert-DiagnosticCamera $cam $other}catch{$failed=$true};Require $failed 'Moved actual diagnostic camera accepted.'
 Require ((Quality-CloudPin $false) -ceq '0' -and $null -eq (Quality-CloudPin $true)) 'Default/quality rendered cloud override policy changed.'
 $cloud=@{overcast=1;rendererSkyAvailable=$true;rendererSky=@{enabled=$true;skyLighting=$true;coverageFromWeather=$true;coveragePinned=$false;coverage=.95;clearCoverage=.05;fullCoverage=.95}}
 Assert-RendererClouds $cloud 1
 foreach($bad in @('unavailable','disabled','legacy','manual','pin','lag','flat','nan')){
  $copy=$cloud|ConvertTo-Json|ConvertFrom-Json
  switch($bad){'unavailable'{$copy.rendererSkyAvailable=$false};'disabled'{$copy.rendererSky.enabled=$false};'legacy'{$copy.rendererSky.skyLighting=$false};'manual'{$copy.rendererSky.coverageFromWeather=$false};'pin'{$copy.rendererSky.coveragePinned=$true};'lag'{$copy.rendererSky.coverage=.05};'flat'{$copy.rendererSky.clearCoverage=.95};'nan'{$copy.rendererSky.coverage='NaN'}}
  $failed=$false;try{Assert-RendererClouds $copy 1}catch{$failed=$true};Require $failed 'Unavailable/pinned/lagging actual renderer clouds accepted.'
 }
 $cloud.overcast=0;$cloud.rendererSky.coverage=.05;Assert-RendererClouds $cloud 0
 $recipe=Renderer-CloudRecipe $cloud;Assert-CloudRecipe $recipe (Renderer-CloudRecipe $cloud)
 $cloud.rendererSky.fullCoverage=.9;$failed=$false;try{Assert-CloudRecipe $recipe (Renderer-CloudRecipe $cloud)}catch{$failed=$true};Require $failed 'Different dry/wet cloud endpoints admitted as matching lighting.'
 Require ((Uniform-StageCommand 52.0463) -ceq 'player setDir 52.0463') 'Actual settled dry heading staging command changed.'
 $changedHeading=$false;try{Assert-UniformHeading 346.873 52.0463}catch{$changedHeading=$true};Require $changedHeading 'Real rain-wait heading drift accepted as an aligned capture.'
 Assert-UniformHeading 52.04631 52.0463
 $sourcePair=@{sand=@{offset=-.09;world='noe.wrp'};mud=@{offset=-.04;world='noe.wrp'}}
 Assert-UniformSources $sourcePair $sourcePair
 $changedSource=$sourcePair|ConvertTo-Json|ConvertFrom-Json -AsHashtable;$changedSource.mud.offset=-.05
 $failed=$false;try{Assert-UniformSources $sourcePair $changedSource}catch{$failed=$true};Require $failed 'Orientation staging changed physical geometry without rejection.'
 $plan=@(Mud-LightingPlan);Require ($plan.Count -eq 2 -and $plan[0].overcast -eq 1 -and $plan[1].overcast -eq 0) 'Paused mud lighting recipe changed.'
 $lightWet=@{uniform=.9;weather=@{liquidRain=.6};mud=@{wetness=.85}};$receipt=@{lastQueuedRow=@{accumulated=.85;queuedPad1=.85}}
 Require (Mud-LightingWetReady $lightWet $receipt) 'Actually accumulated high wetness refused.'
 $receipt.lastQueuedRow.queuedPad1=0;Require (!(Mud-LightingWetReady $lightWet $receipt)) 'Disabled queued coat admitted as high wetness.'
 $receipt.lastQueuedRow.queuedPad1=.85;$receipt.lastQueuedRow.accumulated=.4;Require (!(Mud-LightingWetReady $lightWet $receipt)) 'Pinned high upload substituted for natural terrain history.'
 $receipt.lastQueuedRow.accumulated=.8000;$receipt.lastQueuedRow.queuedPad1=.8000;Require (!(Mud-LightingWetReady $lightWet $receipt)) 'Rounded below-threshold history admitted as actual >=.8.'
 $sunFixture=@{sunAvailable=$true;sunTravelY=-.6;overcast=0;nightEffect=0;solarDrying=.6};Assert-MudSun $sunFixture 16 0
 $sunFixture.overcast=1;$sunFixture.solarDrying=0;Assert-MudSun $sunFixture 16 1
 foreach($bad in @('sun','night','cloud','solar')){
  $copy=$sunFixture|ConvertTo-Json|ConvertFrom-Json
  switch($bad){'sun'{$copy.sunTravelY=.6};'night'{$copy.nightEffect=1};'cloud'{$copy.overcast=0};'solar'{$copy.solarDrying=.6}}
  $failed=$false;try{Assert-MudSun $copy 16 1}catch{$failed=$true};Require $failed 'Incorrect actual mud sunlight accepted.'
 }
 Require (((Mud-RestoreDateTuple 16.125) -join ',') -ceq '1985,6,21,16,7') 'Owned date restoration tuple changed.'
 $failed=$false;try{Mud-RestoreDateTuple 23}catch{$failed=$true};Require $failed 'Unknown date rollover accepted as owned fixture date.'
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
if($WetSoilFragmentViews){$MudLightingViews=[switch]$true}
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
$result.mudLightingViews=[bool]$MudLightingViews
$result.wetSoilFragmentViews=[bool]$WetSoilFragmentViews
$qualityLighting=[bool]($UniformLightingViews -or $MudLightingViews)
$result.renderedCloudRecipe=@{qualityLighting=$qualityLighting;inheritedCoveragePin=[Environment]::GetEnvironmentVariable('WGR_CLOUD_COVERAGE','Process');appliedCoveragePin=(Quality-CloudPin $qualityLighting);scope='Quality views clear the coverage pin so ordinary renderer weather-drive settings can apply. Simulation overcast alone is not proof of rendered cloud coverage, GPU acceptance or image lighting.'}
if($UniformLightingViews){. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1');$result.helperSha256['TerrainPuddleStockRoof.ps1']=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash}
if($SandSnapshotTrace){$result.helperSha256['Read-SandUploadSnapshot.ps1']=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Read-SandUploadSnapshot.ps1')).Hash}
$settings=@{POSEIDON_PLAYER_GROUND_TRACE='1';POSEIDON_UNIFORM_WET_TRACE='1';POSEIDON_SAND_TRACE='1';POSEIDON_MUD_TRACE='1';WGR_UNIFORM_WET='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_LOD_GOVERNOR_RANGE='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
if($qualityLighting){$settings.Remove('WGR_CLOUD_COVERAGE')}
if($WetSoilFragmentViews){$settings.WGR_WET_SOIL_DIAGNOSTIC='1'}
if($TerrainWetnessTrace -or $MudLightingViews){$settings.WGR_TERRAIN_PUDDLE_FIXTURE='1';$result.terrainPuddlesInheritedControl=[Environment]::GetEnvironmentVariable('WGR_TERRAIN_PUDDLES','Process')}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','WGR_SAND_UPLOAD_TRACE','WGR_SAND_SNAPSHOT_FILE','WGR_SAND_SNAPSHOT_ROOT','WGR_CLOUD_COVERAGE')
if($UniformLightingViews){$clear+=@('POSEIDON_HEADLAMP','POSEIDON_LAMP_LIGHTS','POSEIDON_LAMP_RADIUS_SCALE','POSEIDON_LAMP_BRIGHTNESS_SCALE','POSEIDON_LAMP_COLOR','POSEIDON_LAMP_AMBIENT_SCALE','POSEIDON_LAMP_CONE','POSEIDON_LAMP_SPILL','POSEIDON_LAMP_SPILL_RADIUS','POSEIDON_LAMP_SPILL_AMBIENT')}
$clear+=@('WGR_WET_SOIL_DIAGNOSTIC')
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
function Wait-QualityClouds($baseline,[double]$overcast){
 $until=[DateTime]::UtcNow.AddSeconds(3);$lastFailure=''
 do{
  $actual=State;Assert-Paused $baseline $actual
  try{Assert-RendererClouds $actual.weather $overcast;return $actual}catch{$lastFailure=$_.Exception.Message}
  Require ([DateTime]::UtcNow -lt $until) ('Bounded producer cloud readback did not settle: '+$lastFailure)
  Start-Sleep -Milliseconds 100
 }while($true)
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
function Wet-SoilFragmentViews($baseline,$sample,$site,[double]$overcast,[string]$name){
 $initial=Send @{cmd='dev_wet_soil_diagnostic';action='state'};Require ($initial.mode -eq 0) 'Actual diagnostic did not begin in normal mode.'
 $record=@{initial=$initial;views=@{};legend=(Wet-SoilModes);scope='Actual fragment-derived colour channels after production admission; retained water stays drawn and marks actual survivors magenta with its original alpha. Tone mapping/postprocess preclude numeric pixel decoding; overlay can obscure terrain channels. Mode readback is producer queue state, not GPU completion.'}
 $record.refusedModes=@()
 foreach($invalid in @(-1,.5,5)){
  $refused=$false;try{$null=Send @{cmd='dev_wet_soil_diagnostic';action='set';mode=$invalid}}catch{$refused=$true};Require $refused 'Invalid actual diagnostic enum was accepted.'
  Health;$unchanged=Send @{cmd='dev_wet_soil_diagnostic';action='state'};Require ($unchanged.mode -eq 0) 'Refused enum mutated producer mode.';Assert-DiagnosticCamera $initial.camera $unchanged.camera
  $record.refusedModes+=@{requested=$invalid;after=$unchanged}
 }
 try{
  foreach($mode in (Wet-SoilModes)){
   $request=Send @{cmd='dev_wet_soil_diagnostic';action='set';mode=$mode.mode};Require ($request.mode -eq $mode.mode) 'Diagnostic producer request refused.'
   Assert-DiagnosticCamera $initial.camera $request.camera
   $before=State;Assert-Paused $baseline $before;Assert-MudSun $before.weather (Number (Eval 'dayTime')) $overcast;Assert-RendererClouds $before.weather $overcast
   Capture ('mud-fragment-'+$name+'-'+$mode.name)
   $actualMode=Send @{cmd='dev_wet_soil_diagnostic';action='state'};Require ($actualMode.mode -eq $mode.mode) 'Diagnostic producer enum changed during capture.'
   Assert-DiagnosticCamera $initial.camera $actualMode.camera
   $after=State;Assert-Paused $before $after;Assert-MudSun $after.weather (Number (Eval 'dayTime')) $overcast;Assert-RendererClouds $after.weather $overcast
   Assert-CloudRecipe (Renderer-CloudRecipe $baseline.weather) (Renderer-CloudRecipe $after.weather)
   $source=Field 'mud' $sample;Assert-Tuple $source $site $map 'mud'
   foreach($field in @('offset','surfaceY','surfaceDx','surfaceDz')){Require ($source.$field -eq $sample.$field) 'Diagnostic colour mode changed actual source relief.'}
   $record.views[$mode.name]=@{requested=$request;readback=$actualMode;before=$before;after=$after;actualSourceSample=$source;body=(Assert-Body (Lines) ($baseline.time-.65));terrainWetness=(Terrain-WetnessReceipt $after)}
  }
 }finally{
  $reset=Send @{cmd='dev_wet_soil_diagnostic';action='set';mode=0};Require ($reset.mode -eq 0) 'Diagnostic normal mode did not reset.'
 }
 Capture ('mud-fragment-'+$name+'-normal-return')
 $record.after=State;Assert-Paused $baseline $record.after;$record.reset=Send @{cmd='dev_wet_soil_diagnostic';action='state'}
 Require ($record.reset.mode -eq 0) 'Diagnostic normal return enum changed.';Assert-DiagnosticCamera $initial.camera $record.reset.camera
 Assert-MudSun $record.after.weather (Number (Eval 'dayTime')) $overcast;Assert-RendererClouds $record.after.weather $overcast
 $returned=Field 'mud' $sample;Assert-Tuple $returned $site $map 'mud'
 foreach($field in @('offset','surfaceY','surfaceDx','surfaceDz')){Require ($returned.$field -eq $sample.$field) 'Diagnostic normal return changed retained source relief.'}
 return $record
}
function Mud-Lighting($walkRecord,$site){
 Require ($walkRecord.samples.Count -gt 0) 'No actual walked mud contact for lighting comparison.'
 $walked=$walkRecord.samples|Sort-Object offset|Select-Object -First 1
 $contact=Field 'mud' $walked;$centre=Field 'mud' $site
 foreach($sample in @($contact,$centre)){Assert-Tuple $sample $site $map 'mud'}
 Require ($contact.offset -lt -.001) 'Actual walked contact lost its retained relief.'
 $original=State;$originalHour=Number (Eval 'dayTime');$restoreDate=Mud-RestoreDateTuple $originalHour
 $initialReceipt=Terrain-WetnessReceipt $original
 Require (Mud-LightingWetReady $original $initialReceipt) 'Retained mud lighting requires genuinely accumulated/queued >=.8 wetness from the preceding real rain wait.'
 Assert-RendererClouds $original.weather $original.weather.overcast;$cloudRecipe=Renderer-CloudRecipe $original.weather
 $record=@{before=$original;originalHour=$originalHour;originalKnownFixtureDate=$restoreDate;initialTerrainReceipt=$initialReceipt;contact=$contact;centre=$centre;views=@{};
  body=(Assert-Body (Lines) ($original.time-.65));scope='Lighting A/B of retained genuinely rain-wet source-qualified soil. Overcast/date are deliberately set while paused. Rain, wetness and source geometry are preserved. Existing C++ queued parameter receipt is not local fragment rain coverage, Rust acceptance or GPU completion proof.'}
 try{
  Exec 'setDate [1985,6,21,16,0]'
  foreach($light in (Mud-LightingPlan)){
   Exec ('0 setOvercast '+$light.overcast);$null=Wait-QualityClouds $original $light.overcast
   foreach($target in @(@{name='contact';sample=$contact},@{name='centre';sample=$centre})){
    $sample=$target.sample
    foreach($view in (Contact-Poses ([double]$sample.x) ([double]$sample.z) ([double]$sample.surfaceY))){
     $pose=($view.pose|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ' '
     Require ((Eval ('triFreeFlyPose "'+$pose+'"')) -ceq 'OK') 'Paused mud lighting source/contact camera refused.'
     $beforeView=State;Assert-Paused $original $beforeView;$hour=Number (Eval 'dayTime');Assert-MudSun $beforeView.weather $hour $light.overcast
     Assert-RendererClouds $beforeView.weather $light.overcast
     Assert-CloudRecipe $cloudRecipe (Renderer-CloudRecipe $beforeView.weather)
     $terrainBefore=Terrain-WetnessReceipt $beforeView;Require (Mud-LightingWetReady $beforeView $terrainBefore) 'Retained genuinely wet queued substrate was lost.'
     $name=$target.name+'-'+$light.name+'-'+$view.name;Capture ('mud-lighting-'+$name)
     $afterView=State;Assert-Paused $beforeView $afterView;Assert-MudSun $afterView.weather (Number (Eval 'dayTime')) $light.overcast
     Assert-RendererClouds $afterView.weather $light.overcast
     Assert-CloudRecipe $cloudRecipe (Renderer-CloudRecipe $afterView.weather)
     foreach($field in @('sunTravelY','nightEffect','overcast','solarDrying')){Require ($afterView.weather.$field -eq $beforeView.weather.$field) 'Actual mud lighting changed during paused capture.'}
     $actual=Field 'mud' $sample;Assert-Tuple $actual $site $map 'mud'
     foreach($field in @('offset','surfaceY','surfaceDx','surfaceDz')){Require ($actual.$field -eq $sample.$field) 'Mud lighting changed retained source/contact geometry.'}
     $terrainAfter=Terrain-WetnessReceipt $afterView
     Require ($terrainAfter.lastQueuedRow.line -ceq $terrainBefore.lastQueuedRow.line) 'Paused terrain diagnostic advanced unexpectedly.'
     $record.views[$name]=@{before=$beforeView;after=$afterView;hour=$hour;pose=$view.pose;actualSourceSample=$actual;terrainBefore=$terrainBefore;terrainAfter=$terrainAfter}
     if($WetSoilFragmentViews -and $view.name -eq 'low'){$record.views[$name].fragmentDiagnostic=Wet-SoilFragmentViews $afterView $sample $site $light.overcast $name}
    }
   }
  }
 }finally{
  # setDate accepts integer minutes. Restore the owned calendar date, then
  # correct ordinary dayTime readback with the registered skipTime operator.
  Exec ('setDate ['+($restoreDate -join ',')+'];0 setOvercast '+([double]$original.weather.overcast).ToString('R',$culture))
  for($i=0;$i -lt 3;++$i){
   $restoredHour=Number (Eval 'dayTime')
   if($restoredHour -eq $originalHour){break}
   Exec ('skipTime '+($originalHour-$restoredHour).ToString('R',$culture))
  }
 }
 $record.after=Wait-QualityClouds $original $original.weather.overcast;Assert-Paused $original $record.after;$record.restoredHour=Number (Eval 'dayTime')
 Assert-CloudRecipe $cloudRecipe (Renderer-CloudRecipe $record.after.weather)
 Require ($record.restoredHour -eq $originalHour -and [Math]::Abs($record.after.weather.overcast-$original.weather.overcast) -le .001) 'Actual owned date/dayTime or cloud lighting did not restore.'
 return $record
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
 $anchor=@($arm.uniformViewAnchor);$view=Uniform-View $anchor $arm.uniformViewDirection
 for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $original.position[$i])-(Number $anchor[$i])) -le .01) 'Dry/wet actor left the same standing comparison position.'}
 # Ordinary simulation may turn the soldier during real wetting. Stage only
 # world orientation once while paused; never force animation or rewrite history.
 $stageSite=@{x=$original.position[0];z=$original.position[1]};$sourceBefore=@{}
 foreach($kind in @('sand','mud')){$sourceBefore[$kind]=Field $kind $stageSite}
 $stageCommand=Uniform-StageCommand $arm.uniformViewDirection;Exec $stageCommand
 $staged=State;Assert-Paused $original $staged;$stagedHeading=Number (Eval 'getDir player');Assert-UniformHeading $stagedHeading $arm.uniformViewDirection
 $sourceAfter=@{};foreach($kind in @('sand','mud')){$sourceAfter[$kind]=Field $kind $stageSite};Assert-UniformSources $sourceBefore $sourceAfter
 $actor=Eval 'format ["%1",player]';$body=Assert-Body (Lines) ($original.time-.65)
 Require ($actor -is [string] -and ![string]::IsNullOrWhiteSpace($actor) -and $actor -cne '<nullptr-object>') 'Formatted actual controlled player identity is absent.'
 if($band -eq 'dry'){$arm.uniformActorDebug=$actor}else{Require ($actor -ceq $arm.uniformActorDebug) 'Dry/wet actual actor differs.'}
 $record=@{before=$original;orientationStage=@{beforeHeading=$direction;requestedHeading=$arm.uniformViewDirection;command=$stageCommand;readbackHeading=$stagedHeading;after=$staged;sourceBefore=$sourceBefore;sourceAfter=$sourceAfter};actor=@{class=(Eval 'typeOf player');debugName=$actor;model='data3d\mc vojakw2.p3d';modelSource=$body;position=$original.position;direction=$stagedHeading;anchoredDryDirection=$arm.uniformViewDirection;initialPlacementDirectionRequest=90};camera=@{rawEngineView=$view;derivedFromSettledDryDirection=$arm.uniformViewDirection;command='triSetView';effectActive=(Eval 'triGetCameraEffectActive')};views=@{};scope='Same stock standing actor, world orientation staged once while paused to its actual settled dry heading, and anchored render-only camera with matching actual overcast. Rain differs between natural wetting histories. Natural heading and ordinary idle animation may differ; no pose/bone equivalence or pixelwise equality claim.'}
 $lampOwned=$false;$roofOwned=$false
 try{
  # World::SetWeather applies zero-duration overcast immediately. Unlike rain,
  # changing sky/cloud lighting needs no simulation step. Rain history stays real.
  Exec '0 setOvercast 1;setDate [1985,6,21,16,0]';$baseline=Wait-QualityClouds $original 1
  if($band -eq 'dry'){$arm.uniformCloudRecipe=Renderer-CloudRecipe $baseline.weather}else{Assert-CloudRecipe $arm.uniformCloudRecipe (Renderer-CloudRecipe $baseline.weather)}
  $record.producerCloudRecipe=$arm.uniformCloudRecipe
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
   Assert-RendererClouds $stateBefore.weather 1
   Assert-CloudRecipe $arm.uniformCloudRecipe (Renderer-CloudRecipe $stateBefore.weather)
   $headingBefore=Number (Eval 'getDir player');Assert-UniformHeading $headingBefore $arm.uniformViewDirection
   Require ((Eval 'typeOf uniformStockLamp') -ceq 'StreetLampWood') 'Actual lamp identity disappeared before capture.'
   $lampNow=Eval 'getPosASL uniformStockLamp';for($i=0;$i -lt 3;++$i){Require ([Math]::Abs((Number $lampNow[$i])-$lampPosition[$i]) -le .01) 'Actual lamp moved between lighting captures.'}
   Require ((Number (Eval 'triGetCameraEffectActive')) -eq 0) 'Paused render-only camera became a gameplay camera.'
   Capture ('uniform-'+$band+'-'+$name);$stateAfter=State;Assert-Paused $stateBefore $stateAfter
   Assert-RendererClouds $stateAfter.weather 1
   Assert-CloudRecipe $arm.uniformCloudRecipe (Renderer-CloudRecipe $stateAfter.weather)
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
 $record.after=Wait-QualityClouds $original $original.weather.overcast;Assert-Paused $original $record.after
 Assert-CloudRecipe $arm.uniformCloudRecipe (Renderer-CloudRecipe $record.after.weather)
 Require ([Math]::Abs((Number $record.after.weather.overcast)-(Number $original.weather.overcast)) -lt .001) 'Original cloud lighting was not restored.'
 Assert-UniformHeading (Number (Eval 'getDir player')) $stagedHeading
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
  $effectiveMudMinimum=if($MudLightingViews -and $soil){[Math]::Max(.8,$MinimumMudWetness)}else{$MinimumMudWetness}
  $arm.gates.wetWait=@{requestedMinimumMudWetness=$MinimumMudWetness;effectiveMinimumMudWetness=$effectiveMudMinimum;hasAdmittedMudSource=[bool]$soil;wallDeadlineSeconds=$WetDeadlineSeconds;start=State}
  Exec '0 setOvercast 1;0 setRain 1;setAccTime 4';$until=[DateTime]::UtcNow.AddSeconds($WetDeadlineSeconds)
  do{
   Start-Sleep -Milliseconds 400;$wet=State;$arm.gates.wetWait.lastActual=$wet
   $ready=Mud-WetReady $wet ([bool]$soil) $effectiveMudMinimum
   if($MudLightingViews -and $soil){$terrain=Terrain-WetnessReceipt $wet;$arm.gates.wetWait.lastTerrainQueued=$terrain;$ready=$ready -and (Mud-LightingWetReady $wet $terrain)}
   Require ([DateTime]::UtcNow-lt $until) 'Natural liquid rain did not reach actual cloth/GMud/optional queued terrain thresholds within the bounded wetting deadline.'
  }while(!$ready)
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
  if($MudLightingViews){if($soil){$arm.gates.mudLighting=Mud-Lighting $arm.gates.wetSoilWalk $soil}else{$arm.gates.mudLightingUnsupported='No admitted mud source on this original map; no soft-soil lighting acceptance inferred.'}}
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
