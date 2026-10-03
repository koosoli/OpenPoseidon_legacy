<# Actual occupied stock UH60 Dry/Rain diagnostic. Run through with-game-lock.sh.
No GPU waits/readback, synthetic trajectory or in-flight camera override. #>
[CmdletBinding()]
param([ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit='',
 [ValidateRange(20,40)][int]$MeasureSeconds=25,
 [ValidateRange(15,60)][int]$WarmupSeconds=20,
 [ValidateRange(80,220)][double]$HeightAboveBed=120,
 [double]$X=4975,[double]$Z=4675,
 [switch]$IncludeFarCoverOff,
 [switch]$CloudletCosts,
 [switch]$CoarseCosts,
 [switch]$CoarseCapture,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='occupied-rain-flight',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$SelfTest)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Import-Pure([string]$path,[string[]]$names){
 $tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
 Require ($errors.Count -eq 0) ('Helper does not parse: '+$path)
 foreach($name in $names){
  $fn=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
  Require ($fn.Count -eq 1) ('Missing pure helper '+$name)
  # Dot-source in script scope: this import is definitions only, never a runner.
  . ([scriptblock]::Create('function script:'+$name+$fn[0].Extent.Text.Substring(('function '+$name).Length)))
 }
}
$rainHelper=Join-Path $PSScriptRoot 'Test-RainPopulationPerformance.ps1'
$rotorHelper=Join-Path $PSScriptRoot 'Test-RotorLandRuntime.ps1'
$fineHelper=Join-Path $PSScriptRoot 'Test-RainWaterFineRuntime.ps1'
$coarseCaptureHelper=Join-Path $PSScriptRoot 'RainWaterCoarseCapture.ps1'
Import-Pure $fineHelper @('Number','Decode-Eval','Restore-Environment','Complete-LogLines','Read-CompleteLogLines','Assert-Budget')
Import-Pure $rainHelper @('Parse-RainRows')
Import-Pure $rotorHelper @('Assert-RotorState')
Import-Pure $coarseCaptureHelper @('Read-RainWaterCoarseCapture','Assert-RainWaterCoarseCapture')
function Numeric($value){
 Require ($null -ne $value -and $value -is [ValueType] -and $value -isnot [bool]) 'Missing/non-numeric actual field.'
 $n=[double]$value;Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite actual field.';return $n
}
function Stats($values){
 $v=@($values|Sort-Object);Require ($v.Count -gt 0) 'No timing samples.'
 foreach($n in $v){$null=Numeric $n}
 $p95=[int][Math]::Ceiling($v.Count*.95)-1;$mid=[int][Math]::Floor($v.Count/2)
 $median=if($v.Count%2){$v[$mid]}else{($v[$mid-1]+$v[$mid])/2}
 return @{count=$v.Count;average=($v|Measure-Object -Average).Average;median=$median;p95=$v[$p95];max=$v[-1];min=$v[0]}
}
function Published-Stats($values){
 $s=Stats $values
 # No GPU frame ID or timing age is exposed. These are snapshot statistics,
 # never independent-frame quantiles, FPS or verified freshness.
 return @{snapshotCount=$s.count;snapshotMedian=$s.median;snapshotMin=$s.min;snapshotMax=$s.max}
}
function Dry-WeatherRecipe($range,$spacing){
 $range=Numeric $range;$spacing=Numeric $spacing
 Require ($range -ge 2 -and $range -le 16384 -and $range -eq [Math]::Floor($range) -and $spacing -gt 0) 'Invalid native weather-control domain.'
 # RainWaterField::AlignedSourceGrid, including padded equal-area edge nodes.
 $stride=1;while([Math]::Ceiling(($range-1)/$stride)+1 -gt 513){$stride*=2}
 $side=[Math]::Ceiling(($range-1)/$stride)+1;$area=[Math]::Pow($side*$spacing*$stride,2)
 $seconds=360.0;$target=1e-5;$epsilon=1e-6;$rate=.00012;$margin=10.0
 $minimum=$area*$rate*$target*$seconds*$seconds*$margin/$epsilon
 $duration=[Math]::Pow(10,[Math]::Ceiling([Math]::Log10([Math]::Max(1.0,$minimum))))
 $speed=[double][single]([single]$target/[single]$duration)
 $peak=$speed*$seconds*1.1 # conservative allowance for native float accumulation
 $bound=$area*$rate*$peak*($seconds+.25)
 Require ($duration -le 1e30 -and $speed -gt 0 -and $target-$peak -gt 1e-6 -and $bound -le $epsilon/$margin) 'Finite native dry transition does not satisfy unchanged rainfall-budget bound.'
 return @{sourceRange=$range;sourceSpacing=$spacing;stride=$stride;side=$side;allNodeArea=$area;target=$target;durationSeconds=$duration;nativeFloatSpeed=$speed;maximumSimulationSeconds=$seconds;rainMetresPerSecond=$rate;rainBudgetTolerance=$epsilon;maximumDensity=$peak;conservativeWholeArmRainVolume=$bound;
  code=((Number $duration)+' setRain '+(Number $target));scope='Ordinary noncompleted stock weather transition with real automatic particles and tiny positive liquid input; dry coarse physics is initialized, not a no-water-physics zero control'}
}
function Assert-Weather($weather,$particles,[bool]$rain){
 foreach($field in @('overcast','rain','liquidRain','particleDensity','fog','terrainRange','objectRange','selectedRange')){
  $n=Numeric $weather.$field;Require ($n -ge 0) ('Invalid actual weather '+$field)
 }
 Require ($weather.overcast -ge .98 -and $weather.overcast -le 1 -and $weather.fog -le .001) 'Actual matching full cloud/zero fog fixture lost.'
 Require ($particles.mode -ceq 'particle' -and (Numeric $particles.densityOverride) -eq -1 -and $particles.snowflakes -is [bool] -and !$particles.snowflakes) 'Actual automatic stock rain particle parameters are absent.'
 foreach($field in @('weatherRain','effectiveDensity','liquidRain')){Require ((Numeric $particles.$field) -ge 0 -and $particles.$field -le 1) 'Invalid actual particle-state density.'}
 Require ($particles.weatherRain -eq $particles.effectiveDensity -and $particles.liquidRain -eq $particles.weatherRain) 'Actual particle-state liquid density differs from its native weather source.'
 Require ($weather.particleSnowflakes -is [bool] -and !$weather.particleSnowflakes -and $weather.particleDensity -eq $weather.rain) 'Actual effective rendered rain differs from its native weather source.'
 Require ($weather.rain -le 1 -and $weather.liquidRain -eq $weather.rain) 'Actual liquid rain differs from native rain.'
 if($rain){Require ($weather.liquidRain -ge .25) 'Actual rainy arm is not wet.'}
 else{Require ($weather.rain -le .001) 'Dry arm contains actual rain.'}
}
function Assert-Pilot($state,[bool]$moving){
 Assert-RotorState $state
 Require ($state.model -ieq 'data3d\uh-60.p3d' -and $state.local -and $state.engineOn -and !$state.destroyed -and $state.damage -eq 0 -and $state.rpm -ge .85 -and $state.fuel -gt 0) 'Stock local operating helicopter missing.'
 foreach($field in @('driverPresent','driverBrain','driverLocal','driverAlive','driverIsPlayer','pilotIsPlayer','focusIsDriver','driverManual','playerManual','resolvedHelicopter','cameraDistanceValid')){
  Require $state.$field ('Actual occupied manual pilot/view gate lost: '+$field)
 }
 Require (!$state.playerSuspended -and !$state.cameraEffect -and $state.cameraType -eq 2 -and $state.cameraDistance -gt 0 -and $state.cameraDistance -lt 100) 'Ordinary moving external pilot camera is not proved.'
 Require ($state.lookAroundEnabled -and !$state.joystickActive -and !$state.joystickThrustActive -and $state.focusLost -eq 0) 'Unexpected focus/mouse/joystick authority.'
 foreach($field in @('moveUp','moveDown','moveBack','turnLeft','turnRight','moveLeft','moveRight')){
  Require ([Math]::Abs($state.$field) -lt .001) ('Unexpected route pilot input '+$field)
 }
 Require ($state.airborne -and $state.clearance -ge 35 -and $state.clearance -le 300 -and [Math]::Abs($state.speedY) -le 20) 'Actual route no longer has safe airborne altitude/vertical velocity.'
 $speed=[Math]::Sqrt($state.speedX*$state.speedX+$state.speedZ*$state.speedZ)
 Require ($speed -le 100) 'Route exceeded bounded actual velocity.'
 if($moving){Require ($state.moveForward -gt .01 -or $state.fastForward -gt .01) 'Physically held ordinary forward input is absent.'}
 else{Require ($state.moveForward -lt .001 -and $state.fastForward -lt .001 -and $speed -le 2) 'Initial neutral hover velocity/input differs.'}
}
function Initial-LookRearmAllowed($state,[int]$attempts){
 Require ($attempts -ge 0) 'Invalid initial look rearm count.'
 foreach($field in @('lookAroundEnabled','lookAroundToggled','joystickActive','joystickThrustActive')){Require ($state.$field -is [bool]) 'Missing actual initial look authority field.'}
 # Only a lost held Alt during staging is recoverable. Ordinary SDL key input
 # must prove its effect in a later fresh sample; measurement never rearms it.
 return ($attempts -lt 2 -and (Numeric $state.altKey) -eq 0 -and !$state.lookAroundEnabled -and !$state.lookAroundToggled -and
  !$state.joystickActive -and !$state.joystickThrustActive -and (Numeric $state.focusLost) -eq 0)
}
$regionNames=@{84='Weather cover: near cull';85='Weather cover: far cull';86='Weather cover: near draw';87='Weather cover: far draw'}
function Assert-Performance($perf){
 Require ($perf.renderer -is [string] -and $perf.version -is [string] -and $perf.scope -is [string]) 'Published performance metadata absent.'
 foreach($field in @('renderWidth','renderHeight','outputWidth','outputHeight')){Require ((Numeric $perf.$field) -eq $(if($field -match 'Width$'){1280}else{720})) ('Actual native render dimensions differ: '+$field)}
 Require ((Numeric $perf.msaaSamples) -eq 4 -and (Numeric $perf.activeUpscaler) -eq 0) 'Actual MSAA/upscaler differs.'
 Require ($perf.gpuTimestampsAvailable -is [bool] -and $perf.regions.Count -eq 88) 'Complete append-only 88-region timing schema required.'
 for($i=0;$i -lt $perf.regions.Count;$i++){
  $r=$perf.regions[$i];Require ((Numeric $r.index) -eq $i -and $r.name -is [string] -and $r.container -is [bool]) 'Published region identity/order invalid.'
  foreach($field in @('gpuMs','cpuMs')){Require ((Numeric $r.$field) -ge -1) 'Invalid published timing; -1 is unavailable.'}
  $parent=Numeric $r.containedBy;Require ($parent -ge -1 -and $parent -lt 88 -and ($parent -ne $i -or $i -eq 25)) 'Invalid region containment; total is deliberately its own envelope.'
  if($regionNames.ContainsKey($i)){Require ($r.name -ceq $regionNames[$i]) 'Weather region name/index contract changed.'}
 }
 Require ($perf.completedCpuFrames -ge 20 -and $perf.completedCpuFrames -le 256) 'Completed CPU ring has not settled.'
 foreach($field in @('cpuFrameAverageMs','cpuFrameP95Ms','cpuFrameMaxMs')){Require ((Numeric $perf.$field) -gt 0) 'Invalid completed CPU timing window.'}
 Require ($perf.cpuPhases.Count -eq 12) 'Completed CPU phases incomplete.'
 foreach($phase in $perf.cpuPhases){
  Require ($phase.name -is [string]) 'CPU phase identity missing.'
  foreach($field in @('averageMs','p95Ms','maxMs')){Require ((Numeric $phase.$field) -ge 0) 'Invalid completed CPU phase.'}
 }
 if($perf.gpuTimestampsAvailable){Require ($perf.regions[25].name -ceq 'GPU frame total' -and $perf.regions[25].gpuMs -gt 0) 'No actual published GPU frame total.'}
}
function Parse-Capture([string[]]$lines){
 $begins=@();$ends=@();$raw=@();$phases=@();$begun=$false;$ended=$false;$count=0
 foreach($line in $lines){
  if($line -cmatch '\[tri\] triPerfCapture begin n=(\d+) dropped=(\d+)\s*$'){
   Require (!$begun -and !$ended) 'Duplicate/out-of-order capture begin.';$begun=$true;$begins+=,$line;$count=[int]$Matches[1]
   Require ($count -ge 20 -and $count -le 16384 -and [int]$Matches[2] -eq 0) 'Incomplete/dropped frame capture.'
  }elseif($line -cmatch '\[tri\] triPerfCapture offset=(\d+) ms=(.*)\s*$'){
   Require ($begun -and !$ended -and [int]$Matches[1] -eq $raw.Count) 'Frame capture offset gap/order invalid.'
   foreach($value in $Matches[2].Trim().Split(',')){
    Require ($value -cmatch '^\d+\.\d{3}$') 'Unknown capture scalar grammar.'
    $n=[double]::Parse($value,$culture);Require ($n -ge 0) 'Negative frame total.';$raw+=,$n
   }
  }elseif($line -cmatch '\[tri\] triPerfCapture phase=(\S+) avg_ms=([\d.]+) p95_ms=([\d.]+) max_ms=([\d.]+)\s*$'){
   Require ($begun -and !$ended) 'Phase outside capture.'
   $m=$Matches;$phases+=@{name=$m[1];averageMs=[double]::Parse($m[2],$culture);p95Ms=[double]::Parse($m[3],$culture);maxMs=[double]::Parse($m[4],$culture)}
  }elseif($line -cmatch '\[tri\] triPerfCapture end\s*$'){
   Require ($begun -and !$ended) 'Duplicate/out-of-order capture end.';$ended=$true;$ends+=,$line
  }
 }
 Require ($begun -and $ended -and $raw.Count -eq $count) 'Capture totals/end incomplete.'
 $expected=@('setup','sim:step','drw:init','drw:prep','land:gnd','land:obj','drw:land','drw:obj','drw:post','hud','sound','swap')
 Require ($phases.Count -eq 12) 'Captured phase count incomplete.'
 foreach($name in $expected){Require (@($phases|Where-Object{$_.name -ceq $name}).Count -eq 1) ('Captured phase missing/duplicated '+$name)}
 return @{frameCount=$count;dropped=0;rawMs=$raw;summary=(Stats $raw);phases=$phases}
}
function Compare-Routes($dry,$rain){
 foreach($field in @('width','height','spacing')){Require ((Numeric $dry.waterBefore.$field) -eq (Numeric $rain.waterBefore.$field) -and $dry.waterBefore.$field -eq $dry.waterAfter.$field -and $rain.waterBefore.$field -eq $rain.waterAfter.$field) 'Actual dry/rain coarse runoff domains differ or changed during measurement.'}
 Require ($dry.samples.Count -ge 15 -and $rain.samples.Count -ge 15) 'Too few physical route observations.'
 $matches=@()
 foreach($a in $dry.samples){
  $elapsed=$a.state.timeMs-$dry.startMs
  $b=@($rain.samples|Sort-Object {[Math]::Abs(($_.state.timeMs-$rain.startMs)-$elapsed)})[0]
  $dt=[Math]::Abs(($b.state.timeMs-$rain.startMs)-$elapsed)/1000
  if($dt -gt 2){continue}
  $xz=[Math]::Sqrt([Math]::Pow($a.state.x-$b.state.x,2)+[Math]::Pow($a.state.z-$b.state.z,2))
  $h=[Math]::Abs($a.state.y-$b.state.y)
  $velocity=[Math]::Sqrt([Math]::Pow($a.state.speedX-$b.state.speedX,2)+[Math]::Pow($a.state.speedY-$b.state.speedY,2)+[Math]::Pow($a.state.speedZ-$b.state.speedZ,2))
  $matches+=@{dryTimeMs=$a.state.timeMs;rainTimeMs=$b.state.timeMs;elapsedMismatchSeconds=$dt;distanceXZ=$xz;heightDifference=$h;velocityDifference=$velocity;
   overcastDifference=[Math]::Abs($a.weather.overcast-$b.weather.overcast);terrainRangeDifference=[Math]::Abs($a.weather.terrainRange-$b.weather.terrainRange);objectRangeDifference=[Math]::Abs($a.weather.objectRange-$b.weather.objectRange)}
 }
 Require ($matches.Count -ge 15) 'Dry/rain flight windows insufficiently overlap in elapsed time.'
 $summary=@{pairs=$matches;distanceXZ=(Stats @($matches.distanceXZ));heightDifference=(Stats @($matches.heightDifference));velocityDifference=(Stats @($matches.velocityDifference))}
 # Tolerances deliberately permit real physics; output the actual differences,
 # never label identical requested input an identical scene trajectory.
 Require ($summary.distanceXZ.p95 -le 100 -and $summary.heightDifference.p95 -le 30 -and $summary.velocityDifference.p95 -le 20) 'Actual flight overlap is too poor for this bounded diagnostic.'
 Require (($matches.overcastDifference|Measure-Object -Maximum).Maximum -le .01) 'Actual overcast differs between arms; causal comparison refused.'
 return $summary
}
function Parse-CoarseCosts([string[]]$lines){
 $rows=@();$pending=$null;$phases=@('source-sink','clear','raw-transfer','limited-flux','final-cells','fused-flux')
 $finish={param($row)
  Require ($row.phases.Count-eq$row.phaseCount) 'Incomplete final coarse phase receipt.'
  $fused=if($row.phaseCount-eq6){$row.phases[5].calls}else{0}
  Require ($row.phases[2].calls-eq$row.phases[3].calls-and$row.phases[2].calls+$fused-eq$row.steps-$row.empty_steps) 'Actual cached/fused/empty phase calls disagree.'
  $row.fusedFluxCalls=$fused
  return $row
 }
 foreach($line in $lines){
  if($line -match 'Rain water coarse cost: advances='){
   if($null-ne$pending){$rows+=,(& $finish $pending)}
   $pending=@{line=$line;phases=@()}
   $pending.phaseCount=5 # historical schema has no phase_count field
   $countFields=[regex]::Matches($line,'(?:^| )phase_count=(\S+)')
   Require ($countFields.Count-le1) 'Duplicate coarse phase-count field.'
   if($countFields.Count){Require ($countFields[0].Groups[1].Value-ceq'6') 'Unknown coarse phase-count schema.';$pending.phaseCount=6}
   if($line -match '^\[([^\]]+)\]'){$pending.reportLocal=$Matches[1]}
   foreach($key in @('advances','steps','empty_steps','no_step','multi_step','wall_total_ms','wall_max_ms','cpu_samples','cpu_total_ms','cpu_at_wall_max_ms','steps_at_wall_max','slow_50ms')){
    Require ($line -match ('(?:^| )'+$key+'=(-?[0-9]+(?:\.[0-9]+)?)(?: |$)')) ('Missing coarse receipt '+$key)
    $pending[$key]=[double]::Parse($Matches[1],$culture)
    Require ([double]::IsFinite($pending[$key])) 'Nonfinite coarse receipt.'
   }
   Require ($line -match 'scope=coarse-advance-thread-elapsed') 'Wrong coarse timing scope.'
   foreach($key in @('advances','steps','empty_steps','no_step','multi_step','cpu_samples','steps_at_wall_max','slow_50ms')){Require ($pending[$key] -ge 0 -and $pending[$key]%1 -eq 0) 'Invalid coarse integer count.'}
   Require ($pending.advances -gt 0 -and $pending.steps -le 16*$pending.advances -and $pending.empty_steps -le $pending.steps -and $pending.no_step+$pending.multi_step -le $pending.advances -and $pending.cpu_samples -le $pending.advances -and $pending.steps_at_wall_max -le 16 -and $pending.slow_50ms -le $pending.advances) 'Impossible actual coarse counts.'
   Require ($pending.steps -ge $pending.advances-$pending.no_step+$pending.multi_step -and $pending.steps -le $pending.advances-$pending.no_step+15*$pending.multi_step) 'Actual zero/multiple step counts do not bound total steps.'
   Require ($pending.wall_total_ms -ge $pending.wall_max_ms -and $pending.wall_max_ms -ge 0 -and $pending.cpu_total_ms -ge 0 -and ($pending.cpu_at_wall_max_ms -eq -1 -or $pending.cpu_at_wall_max_ms -ge 0)) 'Invalid coarse elapsed/thread timing.'
   Require ($pending.cpu_samples -gt 0 -or ($pending.cpu_total_ms -eq 0 -and $pending.cpu_at_wall_max_ms -eq -1)) 'Unavailable thread samples claimed CPU cost.'
  }elseif($line -match 'Rain water coarse phase: phase=(\S+) calls=(\d+) total_ms=([0-9.]+) max_ms=([0-9.]+) scope=nested-coarse-step'){
   Require ($null -ne $pending) 'Orphan coarse phase report.'
   $name=$Matches[1];$calls=[int64]$Matches[2];$total=[double]::Parse($Matches[3],$culture);$max=[double]::Parse($Matches[4],$culture)
   Require ([double]::IsFinite($total) -and [double]::IsFinite($max)) 'Nonfinite coarse phase timing.'
   $index=$pending.phases.Count
   Require ($index -lt $pending.phaseCount -and $name -ceq $phases[$index] -and $total -ge $max -and $max -ge 0 -and ($calls -gt 0 -or ($total -eq 0 -and $max -eq 0))) 'Coarse phase identity/order/timing mismatch.'
   if($index-in@(0,1,4)){Require ($calls-eq$pending.steps) 'Actual common-phase steps disagree.'}
   else{Require ($calls-le$pending.steps-$pending.empty_steps) 'Actual flux phase exceeds nonempty steps.'}
   $pending.phases+=,@{name=$name;calls=$calls;totalMs=$total;maxMs=$max}
  }elseif($line-match'Rain water coarse phase:'){
   throw 'Malformed coarse phase report.'
  }
 }
 if($null-ne$pending){$rows+=,(& $finish $pending)}
 return $rows
}
if($SelfTest){
 function Refuses([scriptblock]$body){$failed=$false;try{& $body|Out-Null}catch{$failed=$true};Require $failed 'Unsafe fixture accepted.'}
 $recipe=Dry-WeatherRecipe 2048 6.25
 Require ($recipe.side -eq 513 -and $recipe.stride -eq 4 -and $recipe.allNodeArea -eq 164480625 -and $recipe.durationSeconds -eq 1e12 -and $recipe.conservativeWholeArmRainVolume -lt 1e-7) 'Actual Noe finite-transition/budget derivation changed.'
 # Native float recurrence for all 360 seconds at 60 Hz. A completed dry
 # target would trigger the stochastic branch immediately; this never does.
 $density=[single]0;$target=[single]$recipe.target;$speed=[single]$recipe.nativeFloatSpeed;$dt=[single](1.0/60)
 for($i=0;$i -lt 21600;++$i){Require ([Math]::Abs([double]($target-$density)) -ge 1e-6) 'Bounded native float dry transition reached stochastic weather completion.';$density=[single]($density+[single]($speed*$dt))}
 Require ($density -gt 0 -and $density -le $recipe.maximumDensity) 'Native float accumulation exceeds documented dry control.'
 Refuses {Dry-WeatherRecipe 1 6.25};Refuses {Dry-WeatherRecipe 2048 0};Refuses {Dry-WeatherRecipe 2048 ([double]::NaN)}
 $w=[pscustomobject]@{overcast=1.0;rain=0.0;liquidRain=0.0;particleDensity=0.0;particleSnowflakes=$false;fog=0.0;terrainRange=2000.0;objectRange=2000.0;selectedRange=2000.0}
 $particles=[pscustomobject]@{mode='particle';densityOverride=-1.0;snowflakes=$false;weatherRain=0.0;effectiveDensity=0.0;liquidRain=0.0}
 Assert-Weather $w $particles $false;$w.rain=.6;$w.liquidRain=.6;$w.particleDensity=.6;$particles.weatherRain=.6;$particles.effectiveDensity=.6;$particles.liquidRain=.6;Assert-Weather $w $particles $true
 Refuses {Assert-Weather $w $particles $false};$w.overcast=.8;Refuses {Assert-Weather $w $particles $true};$w.overcast=1;$w.particleDensity=0;Refuses {Assert-Weather $w $particles $true};$w.particleDensity=.6
 $particles.densityOverride=0;Refuses {Assert-Weather $w $particles $true};$particles.densityOverride=-1;$particles.mode='off';Refuses {Assert-Weather $w $particles $true};$particles.mode='particle';$particles.effectiveDensity=0;Refuses {Assert-Weather $w $particles $true};$particles.effectiveDensity=.6;$particles.snowflakes=$true;Refuses {Assert-Weather $w $particles $true}
 Refuses {Numeric '1'};Refuses {Numeric ([double]::NaN)}
 $pilot=[ordered]@{}
 foreach($name in @('timeMs','object','candidates','distanceXZ','x','y','z','groundY','clearance','rpm','upX','upY','upZ','speedX','speedY','speedZ','fuel','damage','driverState','moveUp','moveDown','unfocusedMoveUp','focusLost','qKey','airflowScale','cameraDistance','altKey','cameraType','moveForward','fastForward','moveBack','turnLeft','turnRight','moveLeft','moveRight','windX','windY','windZ')){$pilot[$name]=0.0}
 foreach($name in @('readonly','engineOn','destroyed','local','airborne','driverPresent','driverBrain','driverLocal','driverAlive','driverIsPlayer','pilotIsPlayer','focusIsDriver','manual','driverManual','playerManual','playerSuspended','cameraEffect','resolvedHelicopter','airflowEnabled','cameraDistanceValid','mouseTurnActive','lookAroundEnabled','lookAroundToggled','joystickActive','joystickThrustActive','hoverStateAvailable')){$pilot[$name]=$false}
 foreach($name in @('readonly','engineOn','local','airborne','driverPresent','driverBrain','driverLocal','driverAlive','driverIsPlayer','pilotIsPlayer','focusIsDriver','manual','driverManual','playerManual','resolvedHelicopter','cameraDistanceValid','lookAroundEnabled')){$pilot[$name]=$true}
 $pilot.class='UH60';$pilot.model='data3d\uh-60.p3d';$pilot.inputContext='Helicopter';$pilot.resolvedContext='Helicopter';$pilot.inputSeat='driver'
 $pilot.candidates=1.0;$pilot.rpm=1.0;$pilot.fuel=1.0;$pilot.clearance=120.0;$pilot.cameraType=2.0;$pilot.cameraDistance=20.0;$pilot.moveForward=1.0;$pilot.speedZ=20.0
 $pilot=[pscustomobject]$pilot;Assert-Pilot $pilot $true
 foreach($field in @('driverIsPlayer','pilotIsPlayer','resolvedHelicopter','cameraDistanceValid')){$pilot.$field=$false;Refuses {Assert-Pilot $pilot $true};$pilot.$field=$true}
 $pilot.cameraEffect=$true;Refuses {Assert-Pilot $pilot $true};$pilot.cameraEffect=$false
 $pilot.moveForward=0;Refuses {Assert-Pilot $pilot $true};$pilot.speedZ=0;Assert-Pilot $pilot $false
 $pilot.speedZ=3;Refuses {Assert-Pilot $pilot $false}
 $pilot.speedZ=0;$pilot.altKey=0;$pilot.lookAroundEnabled=$false
 Refuses {Assert-Pilot $pilot $false};Require (Initial-LookRearmAllowed $pilot 0) 'Lost held staging Alt cannot be readmitted.'
 Require (!(Initial-LookRearmAllowed $pilot 2)) 'Initial Alt repair is unbounded.'
 foreach($field in @('lookAroundEnabled','lookAroundToggled','joystickActive','joystickThrustActive')){$pilot.$field=$true;Require (!(Initial-LookRearmAllowed $pilot 0)) 'Competing look/input authority accepted.';$pilot.$field=$false}
 $pilot.focusLost=1;Require (!(Initial-LookRearmAllowed $pilot 0)) 'Active focus settling accepted.';$pilot.focusLost=0
 $pilot.altKey=1;Require (!(Initial-LookRearmAllowed $pilot 0)) 'An already held Alt was refreshed.';$pilot.altKey=0
 $pilot.altKey='0';Refuses {Initial-LookRearmAllowed $pilot 0};$pilot.altKey=0
 $pilot.lookAroundEnabled=$true;Assert-Pilot $pilot $false
 $perf=[pscustomobject]@{renderer='wgpu';version='fixture';scope='laggy';renderWidth=1280;renderHeight=720;outputWidth=1280;outputHeight=720;msaaSamples=4;activeUpscaler=0;gpuTimestampsAvailable=$true;regions=@();completedCpuFrames=256;cpuFrameAverageMs=10;cpuFrameP95Ms=12;cpuFrameMaxMs=20;cpuPhases=@()}
 0..87|ForEach-Object{
  $name=if($regionNames.ContainsKey($_)){$regionNames[$_]}elseif($_ -eq 25){'GPU frame total'}else{'region'+$_}
  $perf.regions+= [pscustomobject]@{index=$_;name=$name;gpuMs=1.0;cpuMs=-1.0;containedBy=$(if($_ -eq 25){25}else{-1});container=$false}
 }
 1..12|ForEach-Object{$perf.cpuPhases+=[pscustomobject]@{name=('phase'+$_);averageMs=1.0;p95Ms=2.0;maxMs=3.0}}
 Assert-Performance $perf
 $perf.regions[84].name='Wrong';Refuses {Assert-Performance $perf};$perf.regions[84].name=$regionNames[84]
 $perf.regions[10].containedBy=10;Refuses {Assert-Performance $perf};$perf.regions[10].containedBy=-1
 $perf.regions[25].gpuMs=-1;Refuses {Assert-Performance $perf};$perf.gpuTimestampsAvailable=$false;Assert-Performance $perf
 $perf.activeUpscaler=1;Refuses {Assert-Performance $perf}
 $v=@('[tri] triPerfCapture begin n=20 dropped=0',('[tri] triPerfCapture offset=0 ms='+((@('1.000')*20)-join ',')))
 foreach($name in @('setup','sim:step','drw:init','drw:prep','land:gnd','land:obj','drw:land','drw:obj','drw:post','hud','sound','swap')){$v+='[tri] triPerfCapture phase='+$name+' avg_ms=1.000 p95_ms=1.000 max_ms=1.000'}
 $v+='[tri] triPerfCapture end';$capture=Parse-Capture $v;Require ($capture.frameCount -eq 20 -and $capture.summary.p95 -eq 1) 'Ordered capture parser changed.'
 Refuses {Parse-Capture @($v|ForEach-Object{$_.Replace('dropped=0','dropped=1')})}
 Refuses {Parse-Capture @($v|ForEach-Object{$_.Replace('offset=0','offset=1')})}
 Refuses {Parse-Capture @($v|Select-Object -SkipLast 1)}
 $arm=@{startMs=0;samples=@();waterBefore=@{width=513;height=513;spacing=25};waterAfter=@{width=513;height=513;spacing=25}}
 1..20|ForEach-Object{$arm.samples+=@{state=@{timeMs=$_*1000;x=4975.0;y=150.0;z=4675.0+$_*20;speedX=0.0;speedY=0.0;speedZ=20.0};weather=@{overcast=1.0;terrainRange=2000.0;objectRange=2000.0}}}
 $null=Compare-Routes $arm $arm
 $bad=$arm|ConvertTo-Json -Depth 12|ConvertFrom-Json;$bad.samples[0].weather.overcast=.5
 Refuses {Compare-Routes $arm $bad};$bad=$arm|ConvertTo-Json -Depth 12|ConvertFrom-Json;foreach($q in $bad.samples){$q.state.x+=200}
 Refuses {Compare-Routes $arm $bad}
 $bad=$arm|ConvertTo-Json -Depth 12|ConvertFrom-Json;$bad.waterBefore.spacing=50;Refuses {Compare-Routes $arm $bad}
 $coarse=@('Rain water coarse cost: advances=60 steps=4 empty_steps=1 no_step=56 multi_step=0 wall_total_ms=42.000000 wall_max_ms=12.000000 cpu_samples=60 cpu_total_ms=31.000000 cpu_at_wall_max_ms=0.000000 steps_at_wall_max=1 slow_50ms=0 scope=coarse-advance-thread-elapsed')
 foreach($phase in @('source-sink','clear','raw-transfer','limited-flux','final-cells')){$calls=if($phase -match 'transfer|flux'){3}else{4};$coarse+=('Rain water coarse phase: phase='+$phase+' calls='+$calls+' total_ms=2.000000 max_ms=1.000000 scope=nested-coarse-step')}
 Require (@(Parse-CoarseCosts $coarse).Count -eq 1) 'Complete actual coarse receipt rejected.'
 Refuses {Parse-CoarseCosts ($coarse[0..4])};Refuses {Parse-CoarseCosts ($coarse[1..5])}
 $badCoarse=$coarse.Clone();$badCoarse[3]=$badCoarse[3].Replace('calls=3','calls=4');Refuses {Parse-CoarseCosts $badCoarse}
 $badCoarse=$coarse.Clone();$badCoarse[0]=$badCoarse[0].Replace('steps=4','steps=9999');Refuses {Parse-CoarseCosts $badCoarse}
 $newCoarse=$coarse.Clone();$newCoarse[0]=$newCoarse[0].Replace(' scope=',' phase_count=6 scope=')
 $newCoarse[3]=$newCoarse[3].Replace('calls=3','calls=1');$newCoarse[4]=$newCoarse[4].Replace('calls=3','calls=1')
 $newCoarse+=,'Rain water coarse phase: phase=fused-flux calls=2 total_ms=2.000000 max_ms=1.000000 scope=nested-coarse-step'
 $parsed=@(Parse-CoarseCosts $newCoarse);Require ($parsed.Count-eq1-and$parsed[0].fusedFluxCalls-eq2) 'Exact mixed cached/fused schema rejected.'
 Require (@(Parse-CoarseCosts (@($coarse)+@($newCoarse))).Count-eq2) 'Historical/new reports cannot coexist.'
 Refuses {Parse-CoarseCosts $newCoarse[0..5]} # missing mandatory sixth phase
 Refuses {Parse-CoarseCosts (@($newCoarse)+@($newCoarse[-1]))}
 $badCoarse=$newCoarse.Clone();$badCoarse[6]=$badCoarse[6].Replace('calls=2','calls=3');Refuses {Parse-CoarseCosts $badCoarse}
 $badCoarse=$newCoarse.Clone();$badCoarse[6]=$badCoarse[6].Replace('fused-flux','unknown');Refuses {Parse-CoarseCosts $badCoarse}
 $badCoarse=$newCoarse.Clone();$badCoarse[0]=$badCoarse[0].Replace('phase_count=6','phase_count=7');Refuses {Parse-CoarseCosts $badCoarse}
 $badCoarse=$newCoarse.Clone();$badCoarse[0]=$badCoarse[0].Replace('phase_count=6','phase_count=6 phase_count=6');Refuses {Parse-CoarseCosts $badCoarse}
 $zeroFused=$coarse.Clone();$zeroFused[0]=$zeroFused[0].Replace(' scope=',' phase_count=6 scope=')
 $zeroFused+=,'Rain water coarse phase: phase=fused-flux calls=0 total_ms=0.000000 max_ms=0.000000 scope=nested-coarse-step'
 Require (@(Parse-CoarseCosts $zeroFused).Count-eq1) 'Explicit zero fused phase rejected.'
 $allFused=$zeroFused.Clone()
 foreach($index in @(3,4)){$allFused[$index]=$allFused[$index].Replace('calls=3','calls=0').Replace('total_ms=2.000000 max_ms=1.000000','total_ms=0.000000 max_ms=0.000000')}
 $allFused[6]=$allFused[6].Replace('calls=0','calls=3').Replace('total_ms=0.000000 max_ms=0.000000','total_ms=2.000000 max_ms=1.000000')
 Require (@(Parse-CoarseCosts $allFused)[0].fusedFluxCalls-eq3) 'Fully fused nonempty report rejected.'
 Refuses {Parse-CoarseCosts (@($allFused)+@('Rain water coarse phase: phase=fused-flux calls=garbage'))}
 Write-Output 'Occupied flight performance pure helpers PASS; no game/profile/GPU used.';return
}
Require ([bool]$ExpectedCommit -and [bool]$env:LOCK_OWNER) 'Supply installed ExpectedCommit and invoke with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
function Pair{
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
 Require ($stamp -match ('(?i)\b'+[regex]::Escape($ExpectedCommit)+'[a-f0-9]*\b')) 'Installed source differs.'
 return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
function Same-Pair($a,$b){Require ($a.stamp -ceq $b.stamp -and $a.exe -ceq $b.exe -and $a.dll -ceq $b.dll) 'Installed pair changed.'}
$before=Pair
$campaign=Join-Path $root ('build/rain-helicopter/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force $campaign|Out-Null
$settings=@{POSEIDON_RAIN_TRACE='1';POSEIDON_SNOW_TEST_DEPTH='0';POSEIDON_SNOWLINE='off';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_LOD_GOVERNOR_RANGE='1';POSEIDON_VSYNC='0';POSEIDON_ROTOR_LAND_TRACE='0';WGR_WEATHER_COVER_TRACE='0';POSEIDON_RAIN_WATER_FINE='0';WGR_RAIN_WATER_FINE='0';POSEIDON_SIM_VEHICLE_COST=$(if($CloudletCosts){'1'}else{'0'});POSEIDON_RAIN_WATER_COST=$(if($CoarseCosts){'1'}else{'0'})}
$clear=@('POSEIDON_TEST_RAIN','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','WGR_CLOUD_COVERAGE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_WEATHER_COVER','WGR_WEATHER_COVER_FAR')
$settings.POSEIDON_RAIN_WATER_CAPTURE=$(if($CoarseCapture){'1'}else{'0'})
$keys=@(@($settings.Keys)+$clear+@('POSEIDON_USER_DIR','POSEIDON_RAIN_WATER_CAPTURE_DIR')|Select-Object -Unique);$saved=@{}
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{status='running';installed=$before;sourceHead=(& git -C $root rev-parse HEAD);lockOwner=$env:LOCK_OWNER;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
 helperHashes=@{rain=(Get-FileHash $rainHelper).Hash;rotor=(Get-FileHash $rotorHelper).Hash;fine=(Get-FileHash $fineHelper).Hash;coarseCapture=(Get-FileHash $coarseCaptureHelper).Hash};settings=$settings;cleared=$clear;inherited=$saved;
 recipe=@{x=$X;z=$Z;heightAboveInitialBed=$HeightAboveBed;heading=180;neutralVelocityMax=2;forwardScancode=26;lookScancode=226;measureSimulationSeconds=$MeasureSeconds;warmupSimulationSeconds=$WarmupSeconds};
 scope='Actual occupied manual UH60 route, ordinary external chase camera, stock weather controls and real automatic liquid particles. Dry is a bounded tiny positive noncompleted weather transition with initialized coarse runoff, not a no-water-physics zero control. Completed frame capture is ordered/drop-free. Published GPU/encode regions may lag/duplicate and CPU ring windows overlap: no same-frame completion proof or sum of nested region timings. Requested routes are equal; actual overlap is reported, never exact scene parity.';
 arms=@()}
$p=$null;$client=$null;$held=@()
function Health{
 Require ($p -and !$p.HasExited -and [DateTime]::UtcNow -lt $deadline) 'Owned game exited or bounded wall deadline exceeded.'
 $live=@(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue);Require ($live.Count -eq 1 -and $live[0].Id -eq $p.Id) 'Foreign game contention.'
}
function Send($command){
 Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath $rpc;$writer.WriteLine($line)
 do{$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath $rpc;$reply=$replyLine|ConvertFrom-Json}while($null -eq $reply.ok)
 Require $reply.ok $replyLine;return $reply
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Clock{
 $q=Send @{cmd='query';what='play_state'};Require ($q.has_player -and $q.player_active -and $q.player_local) 'Actual player absent.'
 return [long]$q.time_ms
}
function State{
 $pos=Eval 'getPosASL rotorHeli';Require ($pos.Count -eq 3) 'Actual helicopter locator absent.'
 $s=Send @{cmd='rotor_state';x=[double]$pos[0];z=[double]$pos[1];radius=16};Assert-RotorState $s
 if($null -ne $arm.weatherStartMs){Require ($s.timeMs-$arm.weatherStartMs -ge 0 -and $s.timeMs-$arm.weatherStartMs -le 360000) 'Actual weather control exceeded derived whole-arm360-second simulation bound.'}
 if($null -eq $script:object){$script:object=$s.object};Require ($script:object -eq $s.object) 'Diagnostic switched aircraft.'
 $s|ConvertTo-Json -Depth 8 -Compress|Add-Content -LiteralPath (Join-Path $armDir 'rotor-states.jsonl');return $s
}
function Hold([int]$sc){$null=Send @{cmd='key';sc=$sc;hold=$true};if($script:held -notcontains $sc){$script:held+=,$sc}}
function Release([int]$sc){$null=Send @{cmd='key_up';sc=$sc};$script:held=@($script:held|Where-Object{$_ -ne $sc})}
function Close-Owned{
 foreach($sc in @($script:held)){Release $sc};$null=Send @{cmd='exit'}
 Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit.'
 $meta=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$path=Join-Path $armDir 'lifecycle.json';$meta|ConvertTo-Json|Set-Content -LiteralPath $path
 & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $path -LogPath $log;$arm.lifecycle=$meta
}
try{
 $controls=@('Default');if($IncludeFarCoverOff){$controls+='FarCoverOff'}
 foreach($control in $controls){foreach($weatherName in @('Dry','Rain')){
  Same-Pair $before (Pair);Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Ownership conflict.'
  foreach($key in $keys){Restore-Environment $key $null};foreach($key in $settings.Keys){Restore-Environment $key $settings[$key]}
  if($control -ceq 'FarCoverOff'){$env:WGR_WEATHER_COVER_FAR='0'}
  $armDir=Join-Path $campaign ($control+'-'+$weatherName);$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force $profile|Out-Null
  if($CoarseCapture){$env:POSEIDON_RAIN_WATER_CAPTURE_DIR=[IO.Path]::GetFullPath($armDir)}
  & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
  $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;')))
  $mission=Join-Path $armDir 'ordinary.noe';New-Item -ItemType Directory -Force $mission|Out-Null
  Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
  $env:POSEIDON_USER_DIR=$profile
  $arm=[ordered]@{control=$control;weatherName=$weatherName;status='running';samples=@();staging=@();profileSha256=(Get-FileHash $cfg).Hash;missionSha256=(Get-FileHash (Join-Path $mission 'mission.sqm')).Hash};$result.arms+=,$arm
  $log=Join-Path $armDir 'engine.log';$rpc=Join-Path $armDir 'harness.jsonl';$script:object=$null;$script:held=@()
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
  $deadline=[DateTime]::UtcNow.AddSeconds(360)
  $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError (Join-Path $armDir 'stderr.txt')
  $null=$p.Handle;$arm.pid=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(120)
  do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness startup timeout.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=10000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Stock scene not ready.'
  Exec ('setAccTime 0;player allowDamage false;setDate [1985,6,21,16,0];0 setFog 0;0 setOvercast 1;0 setRain '+$(if($weatherName -ceq 'Rain'){'1'}else{'0'})+';triSetBrightness 1')
  Require ((Eval 'triClearView') -ceq 'OK') 'Render view override could not release.'
  $null=Send @{cmd='dev_snow';action='disable'}
  $arm.initialParticleParams=Send @{cmd='weather_particles';action='set';mode='particle';snowflakes=$false}
  $bed=Send @{cmd='dev_terrain_brush';action='state';x=$X;z=$Z};$null=Numeric $bed.vertexHeight;$arm.nativeBed=$bed
  $policy=Dry-WeatherRecipe $bed.range $bed.spacing;$arm.expectedWaterDomain=@{width=$policy.side;height=$policy.side;spacing=$policy.sourceSpacing*$policy.stride}
  $arm.weatherStartMs=Clock;$arm.weatherBeforeTransition=Send @{cmd='weather_visibility'}
  if($weatherName -ceq 'Dry'){
   Require ($arm.weatherBeforeTransition.rain -eq 0 -and $arm.weatherBeforeTransition.liquidRain -eq 0) 'Dry transition must start from actual native zero rain.'
   $arm.dryWeatherRecipe=$policy;Exec $arm.dryWeatherRecipe.code
  }
  $arm.waterAtWeatherStart=Send @{cmd='dev_rain_water';action='state'};Assert-Budget $arm.waterAtWeatherStart
  $soil=Send @{cmd='dev_mud';action='sample';x=$X;z=$Z};Require ($soil.world.Replace('\','/').ToLowerInvariant() -match '(^|/)noe\.wrp$') 'Actual stock Nogova not mounted.';$arm.world=$soil.world
  $xz=(Number $X)+','+(Number $Z)
  Exec ('rotorHeli="UH60" createVehicle ['+$xz+',0];rotorHeli allowDamage false;rotorPilot=player;rotorPilot moveInDriver rotorHeli;rotorHeli setPos ['+$xz+',0];rotorHeli setDir 180;rotorHeli setFuel 1;rotorHeli engineOn true;rotorPilot action ["AUTOHOVER",rotorHeli];rotorHeli switchCamera "EXTERNAL"')
  Require ((Eval 'typeOf driver rotorHeli') -ceq 'SoldierWB') 'Actual stock controlled soldier not in driver.'
  Exec 'setAccTime 1';$spool=Clock
  do{Start-Sleep -Milliseconds 500;$s=State;$arm.staging+=,$s;Require ((Clock)-$spool -le 35000 -and !$s.destroyed -and $s.damage -eq 0 -and $s.engineOn -and $s.clearance -lt 6) 'Grounded physical spool failed.'}while($s.rpm -lt .85)
  Exec 'setAccTime 0';Hold 226
  # One staging relocation only; no setVelocity, RPM setter or position writes
  # occur after this line. Q briefly refreshes ordinary stock height helper.
  Exec ('rotorHeli setPosASL ['+$xz+','+(Number ([double]$bed.vertexHeight+$HeightAboveBed))+'];rotorHeli flyInHeight '+(Number $HeightAboveBed))
  Hold 20;Exec 'setAccTime 1';Start-Sleep -Milliseconds 250;Release 20
  $warmStart=Clock
  do{Start-Sleep -Seconds 1;$s=State;$arm.staging+=,$s;Require (!$s.destroyed -and $s.damage -eq 0) 'Physical warmup lost aircraft.'}while((Clock)-$warmStart -lt $WarmupSeconds*1000)
  $readyStart=Clock;$lookRearms=0;$arm.readiness=@()
  do{
   $w=Send @{cmd='weather_visibility'};$particles=Send @{cmd='weather_particles';action='state'};$s=State;$arm.staging+=,$s;$ready=$false
   $attempt=[ordered]@{state=$s;weather=$w;particleParams=$particles;accepted=$false;rejection=$null;lookRearm=$false}
   try{Assert-Weather $w $particles ($weatherName -ceq 'Rain');Assert-Pilot $s $false;$ready=$true;$attempt.accepted=$true}catch{
    $attempt.rejection=$_.Exception.Message
    if($attempt.rejection -ceq 'Unexpected focus/mouse/joystick authority.' -and (Initial-LookRearmAllowed $s $lookRearms)){
     Hold 226;$lookRearms++;$attempt.lookRearm=$true
    }
   }
   $arm.readiness+=,$attempt
   if(!$ready){Require ((Clock)-$readyStart -lt 30000) ('Natural matching weather and neutral occupied hover did not settle: '+$attempt.rejection);Start-Sleep -Seconds 1}
  }while(!$ready)
  $arm.initial=$s;$arm.readyWeather=$w;$arm.readyParticleParams=$particles;$arm.warmupActualMs=$s.timeMs-$warmStart
  Require ([Math]::Sqrt([Math]::Pow($s.x-$X,2)+[Math]::Pow($s.z-$Z,2)) -le 8 -and [Math]::Abs($s.y-([double]$bed.vertexHeight+$HeightAboveBed)) -le 15) 'Actual initial hover left requested bounded XZ/altitude recipe.'
  $arm.waterBefore=Send @{cmd='dev_rain_water';action='state'}
  Assert-Budget $arm.waterBefore
  foreach($field in @('width','height','spacing')){Require ((Numeric $arm.waterBefore.$field) -eq $arm.expectedWaterDomain.$field) 'Actual stable coarse runoff domain differs from the native aligned source.'}
  Hold 26;$leadStart=Clock
  do{Start-Sleep -Seconds 1;$s=State;Assert-Pilot $s $true}while((Clock)-$leadStart -lt 5000)
  $initialPerf=Send @{cmd='frame_performance'};Assert-Performance $initialPerf;$arm.initialPerformance=$initialPerf
  $firstRainRows=@(Parse-RainRows @(Read-CompleteLogLines $log)).Count
  Require ((Eval 'triPerfCapture 16384') -ceq 'OK') 'Frame capture failed to arm.'
  $arm.startMs=Clock;$previous=$null
  do{
   # Deliberately at most one published-region poll per wall second; no waits
   # for GPU completion or counter readback. Save exact poll/state clocks.
   Start-Sleep -Seconds 1;Health;$queryStart=[DateTime]::UtcNow
   $s=State;Assert-Pilot $s $true;$w=Send @{cmd='weather_visibility'};$particles=Send @{cmd='weather_particles';action='state'};Assert-Weather $w $particles ($weatherName -ceq 'Rain')
   $perf=Send @{cmd='frame_performance'};Assert-Performance $perf
   Require ($perf.version -ceq $initialPerf.version -and $perf.renderer -ceq $initialPerf.renderer -and $perf.gpuTimestampsAvailable -eq $initialPerf.gpuTimestampsAvailable) 'Actual device/version/timestamp capability changed.'
   if($previous){
    $dt=($s.timeMs-$previous.timeMs)/1000;Require ($dt -gt 0 -and $dt -le 10) 'Route state clock gap/reversal.'
    $distance=[Math]::Sqrt([Math]::Pow($s.x-$previous.x,2)+[Math]::Pow($s.y-$previous.y,2)+[Math]::Pow($s.z-$previous.z,2))
    Require ($distance -le 110*$dt+5) 'Physical trajectory discontinuity exceeds bounded velocity.'
   }
   $entry=@{wallUtc=$queryStart.ToString('o');pollWallMs=([DateTime]::UtcNow-$queryStart).TotalMilliseconds;state=$s;weather=$w;particleParams=$particles;performance=$perf}
   $arm.samples+=,$entry;$entry|ConvertTo-Json -Depth 12 -Compress|Add-Content -LiteralPath (Join-Path $armDir 'samples.jsonl');$previous=$s
  }while($s.timeMs-$arm.startMs -lt $MeasureSeconds*1000)
  Require ((Eval 'triPerfCapture 0') -ceq 'OK') 'Frame capture dropped or incomplete.'
  $arm.endMs=Clock;Release 26;Exec 'setAccTime 0'
  Require ($arm.endMs-$arm.startMs -ge $MeasureSeconds*1000 -and $arm.samples.Count -ge 15) 'Physical measurement duration/samples insufficient.'
  $end=$arm.samples[-1].state
  $arm.displacementXZ=[Math]::Sqrt([Math]::Pow($end.x-$arm.initial.x,2)+[Math]::Pow($end.z-$arm.initial.z,2))
  Require ($arm.displacementXZ -ge 50) 'No actual moving helicopter route exercised.'
  $arm.waterAfter=Send @{cmd='dev_rain_water';action='state'}
  Assert-Budget $arm.waterAfter
  $arm.measuredRainInputVolume=$arm.waterAfter.rainVolume-$arm.waterBefore.rainVolume
  if($weatherName -ceq 'Dry'){Require ([Math]::Abs($arm.waterAfter.rainVolume-$arm.waterAtWeatherStart.rainVolume) -le .000001) 'Whole dry physical arm exceeded unchanged rainfall-input budget.'}
  if($weatherName -ceq 'Rain'){Require ($arm.measuredRainInputVolume -gt 0) 'Actual rain simulation did not accumulate real liquid input.'}
  else{Require ([Math]::Abs($arm.measuredRainInputVolume) -le .000001) 'Dry physical arm accumulated liquid rain input.'}
  if($CoarseCapture){
   # Frame capture stopped and world paused above; no dump/readback in timing.
   $frozenTime=Clock;$frozenField=Send @{cmd='dev_rain_water';action='state'}
   $receipt=Send @{cmd='dev_rain_water';action='coarse_capture'}
   $expectedCapturePath=Join-Path $armDir 'coarse-runoff.rwcap'
   $header=Read-RainWaterCoarseCapture $expectedCapturePath
   $returned=Send @{cmd='dev_rain_water';action='state'}
   Require ((Clock)-eq$frozenTime) 'Coarse export advanced the actual paused clock.'
   Assert-RainWaterCoarseCapture $receipt $header $frozenField $returned $expectedCapturePath $frozenTime
   $captureHash=(Get-FileHash -LiteralPath $expectedCapturePath).Hash
   $duplicateRefusal=$null
   try{$null=Send @{cmd='dev_rain_water';action='coarse_capture'}}catch{$duplicateRefusal=$_.Exception.Message}
   Require ($duplicateRefusal -match 'unused one-time gate') 'Actual duplicate capture did not prove the one-time refusal.'
   Require ((Get-FileHash -LiteralPath $expectedCapturePath).Hash -ceq $captureHash -and (Clock) -eq $frozenTime) 'Refused duplicate changed file or paused clock.'
   $duplicateState=Send @{cmd='dev_rain_water';action='state'}
   Assert-RainWaterCoarseCapture $receipt $header $frozenField $duplicateState $expectedCapturePath $frozenTime
   $arm.coarseCapture=@{receipt=$receipt;header=$header;before=$frozenField;after=$duplicateState;sha256=$captureHash;duplicateRefusal=$duplicateRefusal;scope='Exact raw bed/depth copied once after completed frame capture, with actual clock paused; no measured-window allocation or state mutation.'}
  }
  $null=Send @{cmd='screenshot';path=(Join-Path $armDir 'route-end.png')}
  Close-Owned;Same-Pair $before (Pair);$client.Dispose();$client=$null;$p=$null
  $lines=@(Read-CompleteLogLines $log)
  $arm.capture=Parse-Capture $lines
  $arm.rainRows=@(Parse-RainRows $lines|Select-Object -Skip $firstRainRows)
  if($weatherName -ceq 'Rain'){Require ($arm.rainRows.Count -ge 15 -and @($arm.rainRows|Where-Object{$_.drops -gt 0 -and $_.drawn -gt 0}).Count -ge 10) 'Actual live/rendered rain trace absent during flight.'}
  $allLogs=@($lines)+@(Read-CompleteLogLines (Join-Path $armDir 'stdout.txt'))+@(Read-CompleteLogLines (Join-Path $armDir 'stderr.txt'))
  $allLogs=$allLogs -join "`n"
  if($CloudletCosts){Require ($allLogs -match 'Sim cloudlet cost over \d+ ticks: stage=cloudlets.runoff-advance') 'Requested actual nested runoff CPU accounting is absent.'}
  if($CoarseCosts){$arm.coarseCosts=@(Parse-CoarseCosts $lines);Require ($arm.coarseCosts.Count -ge 15 -and ($arm.coarseCosts.steps|Measure-Object -Sum).Sum -gt 0) 'Requested actual coarse steps/phase receipts absent.'}
  Require ($allLogs -notmatch 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator') 'Runtime/script/renderer failure.'
  if($control -ceq 'FarCoverOff'){Require ($allLogs -match 'weather cover far cascade: enabled=false' -and $allLogs -notmatch 'weather cover far map:') 'Production far-cover OFF not proved.'}
  else{Require ($allLogs -match 'weather cover far cascade: enabled=true') 'Production default far-cover not proved.'}
  # Moving-map trace emits whenever its camera matrix changes and would bias
  # this benchmark. Actual encode rows prove the named depth pass ran; this
  # does not claim that a timing snapshot certifies its sampled image readiness.
  if($weatherName -ceq 'Rain'){
   Require (@($arm.samples|Where-Object{$_.performance.regions[86].cpuMs -ge 0}).Count -gt 0) 'No actual recorded near weather depth timing in rainy flight.'
   if($control -ceq 'Default'){Require (@($arm.samples|Where-Object{$_.performance.regions[87].cpuMs -ge 0}).Count -gt 0) 'No actual recorded far weather depth timing in rainy flight.'}
  }
  $arm.publishedRegionSummary=@()
  for($i=0;$i -lt 88;$i++){
   $observed=@($arm.samples|ForEach-Object{$_.performance.regions[$i]})
   $row=@{index=$i;name=$observed[0].name;containedBy=$observed[0].containedBy;container=$observed[0].container;
    scope='Last nonblocking published values sampled once per wall second; possible duplicate/lagged frames, not independent per-frame GPU samples.'}
   foreach($field in @('gpuMs','cpuMs')){$valid=@($observed|Where-Object{$_.$field -ge 0}|ForEach-Object{[double]$_.$field});$row[$field]=if($valid.Count){Published-Stats $valid}else{$null}}
   $arm.publishedRegionSummary+=,$row
  }
  $arm.status='actual-occupied-flight-captured'
 }}
 $result.comparisons=@()
 foreach($control in $controls){
  $dry=@($result.arms|Where-Object{$_.control -ceq $control -and $_.weatherName -ceq 'Dry'})[0]
  $rain=@($result.arms|Where-Object{$_.control -ceq $control -and $_.weatherName -ceq 'Rain'})[0]
  Require ($dry.profileSha256 -ceq $rain.profileSha256 -and $dry.missionSha256 -ceq $rain.missionSha256) 'Requested graphics/mission differ.'
  $overlap=Compare-Routes $dry $rain
  $result.comparisons+=@{control=$control;actualOverlap=$overlap;cpuFrameAverageDeltaMs=$rain.capture.summary.average-$dry.capture.summary.average;
   cpuFrameP95DeltaMs=$rain.capture.summary.p95-$dry.capture.summary.p95;
   scope='Bounded two-run differential with measured physical overlap, not exact scene parity or causal isolation of individual GPU regions. Actual visibility ranges and liquid history retained.'}
 }
 $result.status='actual-occupied-dry-rain-flight-measured'
}catch{$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=$_.InvocationInfo.PositionMessage}
finally{
 if($p -and !$p.HasExited -and $client){try{Close-Owned}catch{$result.cleanupError=$_.Exception.Message;$result.status='failed'}}
 if($client){$client.Dispose()}
 if($p -and !$p.HasExited){$result.status='failed';$result.forcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$null=$p.WaitForExit(10000)}
 foreach($key in $keys){Restore-Environment $key $saved[$key]}
 try{$result.after=Pair;Same-Pair $before $result.after}catch{$result.status='failed';$result.provenanceError=$_.Exception.Message}
 $result|ConvertTo-Json -Depth 32|Set-Content -LiteralPath (Join-Path $campaign 'result.json')
 Write-Output "Occupied rain flight record: $campaign"
}
Require ($result.status -ceq 'actual-occupied-dry-rain-flight-measured') 'Occupied rain flight campaign failed; inspect retained record.'
