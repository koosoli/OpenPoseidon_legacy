# Read-only observations from completed installed-game natural-water campaigns.
[CmdletBinding()]
param([string]$Baseline='', [string]$Candidate='',
 [ValidateSet('noe','eden')][string]$Map='noe', [string]$Output='', [switch]$SelfTest)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Finite($value){
 Require ($null-ne$value) 'Missing numeric observation.'
 $n=[double]$value;Require (![double]::IsNaN($n)-and![double]::IsInfinity($n)) 'Nonfinite observation.';return $n
}
function Key([double]$x,[double]$z){return $x.ToString('R',$culture)+':'+$z.ToString('R',$culture)}
function GeometryHash($vertices){
 # Hash native geometry only. Installed stamp, clock, tide and process address
 # are legitimate fresh-session differences, not changes to authored vertices.
 $keys=[string[]]@($vertices.Keys);[Array]::Sort($keys,[StringComparer]::Ordinal)
 $stream=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($stream)
 $sha=[Security.Cryptography.SHA256]::Create()
 try{foreach($key in $keys){$xy=$key.Split(':');$writer.Write([double]::Parse($xy[0],$culture));$writer.Write([double]::Parse($xy[1],$culture));$writer.Write([single](Finite $vertices[$key]))}
  $writer.Flush();return [BitConverter]::ToString($sha.ComputeHash($stream.ToArray())).Replace('-','')
 }finally{$sha.Dispose();$writer.Dispose();$stream.Dispose()}
}
function SeaSource($nativeSea,$fieldSea){
 $a=Finite $nativeSea;$b=Finite $fieldSea
 Require ($a-eq$b) 'Native corpus sea differs from its actual same-run fine field.'
}
function Distribution($values){
 $sorted=@($values|Sort-Object);if(!$sorted.Count){return @{count=0;mean=$null;rms=$null;p95=$null;maximum=$null;over30mm=0;over60mm=0}}
 $sum=0.0;$squares=0.0;$over30=0;$over60=0
 foreach($v in $sorted){$n=Finite $v;$sum+=$n;$squares+=$n*$n;if($n-gt.03){++$over30};if($n-gt.06){++$over60}}
 return @{count=$sorted.Count;mean=$sum/$sorted.Count;rms=[Math]::Sqrt($squares/$sorted.Count);
  p95=$sorted[[Math]::Max(0,[int][Math]::Ceiling($sorted.Count*.95)-1)];maximum=$sorted[-1];over30mm=$over30;over60mm=$over60}
}
function Edges($cells,$vertices){
 $edges=[Collections.Generic.List[object]]::new()
 foreach($p in $cells.Values){foreach($axis in @('x','z')){
  $x=$p.x;$z=$p.z
  if($axis-ceq'x'){$x+=6.25;$f0=$vertices[(Key ($p.x+3.125) ($p.z-3.125))];$f1=$vertices[(Key ($p.x+3.125) ($p.z+3.125))]}
  else{$z+=6.25;$f0=$vertices[(Key ($p.x-3.125) ($p.z+3.125))];$f1=$vertices[(Key ($p.x+3.125) ($p.z+3.125))]}
  $q=$cells[(Key $x $z)]
  if($null-eq$q){continue};Require ($null-ne$f0-and$null-ne$f1) 'Missing actual native shared face.'
  $bothWet=$p.depth-gt.001-and$q.depth-gt.001
  $minimumFaceDepth=[Math]::Min($p.head,$q.head)-[Math]::Max($f0,$f1)
  $edges.Add(@{key=$p.key+'/'+$axis;a=$p.key;b=$q.key;x=$p.x;z=$p.z;axis=$axis;
   delta=[Math]::Abs($p.head-$q.head);bothWet=$bothWet;fullyWet=$bothWet-and$minimumFaceDepth-gt.001;
   face0=$f0;face1=$f1;minimumFaceDepth=$minimumFaceDepth;
   flowJump=[Math]::Sqrt([Math]::Pow($p.flowX-$q.flowX,2)+[Math]::Pow($p.flowZ-$q.flowZ,2))})
 }}
 return ,$edges.ToArray()
}
function Summary($cells,$edges){
 $wet=@($edges|Where-Object bothWet);$full=@($edges|Where-Object fullyWet)
 $flowStats=Distribution @($wet|ForEach-Object flowJump)
 $flowStats.Remove('over30mm');$flowStats.Remove('over60mm');$flowStats.unit='metres-per-second'
 $curvature=[Collections.Generic.List[double]]::new();$extrema=0
 foreach($p in $cells.Values){
  if($p.depth-le.001){continue};$neighbours=@()
  foreach($offset in @(@(-6.25,0),@(6.25,0),@(0,-6.25),@(0,6.25))){
   $q=$cells[(Key ($p.x+$offset[0]) ($p.z+$offset[1]))];if($null-ne$q-and$q.depth-gt.001){$neighbours+=,$q}
  }
  if($neighbours.Count-ne4){continue};$sum=0.0;$lo=[double]::PositiveInfinity;$hi=[double]::NegativeInfinity
  foreach($q in $neighbours){$sum+=$q.head;$lo=[Math]::Min($lo,$q.head);$hi=[Math]::Max($hi,$q.head)}
  $curvature.Add([Math]::Abs($p.head-$sum/4))
  if($p.head-lt$lo-or$p.head-gt$hi){++$extrema}
 }
 return @{cells=$cells.Count;wetCentres=@($cells.Values|Where-Object{$_.depth-gt.001}).Count;
  wetEdges=(Distribution @($wet|ForEach-Object delta));fullyWetNativeFaces=(Distribution @($full|ForEach-Object delta));
  wetInteriorCurvature=(Distribution $curvature.ToArray());wetInteriorExtrema=$extrema;
  flowJumps=$flowStats;
  largestFullyWetSteps=@($full|Sort-Object delta -Descending|Select-Object -First 8)}
}
function Frozen($a,$b){
 Require ($a.time-eq$b.time) 'Centre/native sample advanced the actual paused clock.'
 foreach($field in @('revision','generation','volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume','pendingSeconds','fineTiles')){
  Require ($null-ne$a.field.$field-and$a.field.$field-eq$b.field.$field) ('Paused water changed/missing '+$field)
 }
 Require ($a.source.heightRevision-eq$b.source.heightRevision-and!$a.source.baseline-and!$b.source.baseline) 'Source changed or edited.'
 foreach($field in @('rain','liquidRain','particleDensity','particleSnowflakes')){Require ($null-ne$a.weather.$field-and$a.weather.$field-eq$b.weather.$field) ('Paused weather changed/missing '+$field)}
}
function ReadSnapshot([string]$path){
 Require (Test-Path -LiteralPath $path -PathType Leaf) ('Missing completed result: '+$path)
 $r=Get-Content -Raw -LiteralPath $path|ConvertFrom-Json;$arm=$r.maps.$Map.arms.on
 Require ($r.status-match'passed'-and$arm.status-match'passed'-and$arm.cpuFine-and$arm.rendererFine) 'Not a completed actual fine ON campaign.'
 Require ($arm.lifecycle.status-ceq'ok'-and$arm.lifecycle.exit_code-eq0-and!$arm.lifecycle.timed_out-and!$arm.lifecycle.contended) 'Campaign lacks uncontended normal exit.'
 Require ($r.installed.stamp-ceq$r.after.stamp-and$r.installed.exe-ceq$r.after.exe-and$r.installed.dll-ceq$r.after.dll) 'Installed pair changed during campaign.'
 $d=$arm.gates.depths;$descriptor=$arm.gates.sampledOwnerNative
 Require ($d.owner.complete-and$d.owner.finite-and$d.owner.sourceCurrent-and$d.owner.cells-eq1024-and$d.owner.size-eq200) 'Incomplete native owner sample.'
 Frozen $d.before $d.after
 Require ((Get-FileHash -LiteralPath $descriptor.path -Algorithm SHA256).Hash-ceq$descriptor.sha256) 'Recorded native corpus hash mismatch.'
 $native=Get-Content -Raw -LiteralPath $descriptor.path|ConvertFrom-Json
 Require ($native.side-eq33-and$native.order-ceq'z-major then x'-and$native.vertices.Count-eq1089-and
  $native.header.readonly-and$native.header.baseline-ceq$false-and$native.header.ok-and$native.header.fineSourceReady-and
  $native.header.range-eq2048-and$native.header.spacing-eq6.25-and$native.source.world-ceq$Map-and
  $native.source.wrpSha256-match'^[a-fA-F0-9]{64}$') 'Native corpus lacks genuine unedited source/domain.'
 Require ($native.installed.stamp-ceq$r.installed.stamp-and$native.header.heightRevision-eq$d.before.source.heightRevision) 'Native source and sample provenance differ.'
 SeaSource $native.header.seaLevel $arm.gates.wetFine.seaLevel
 Frozen $native.before $native.after
 Require ($native.before.time-eq$d.before.time-and$native.before.field.revision-eq$d.before.field.revision) 'Native corners and water sample are from different states.'
 Require ($native.origin[0]-eq$d.owner.x-and$native.origin[1]-eq$d.owner.z) 'Native corners do not cover the sampled owner.'
 $vertices=@{};for($i=0;$i-lt1089;++$i){
  $v=$native.vertices[$i];Require ($v.Count-eq3-and$v[0]-eq$native.origin[0]+($i%33)*6.25-and$v[1]-eq$native.origin[1]+[Math]::Floor($i/33)*6.25) 'Native vertex order/domain mismatch.'
  $vertices[(Key $v[0] $v[1])]=Finite $v[2]
 }
 Require ($d.points.Count-eq1024) 'Missing actual native centres.';$cells=@{}
 foreach($point in $d.points){
  $p=$point.sample;$x=Finite $point.x;$z=Finite $point.z;$key=Key $x $z
  $gx=($x-$d.owner.x-3.125)/6.25;$gz=($z-$d.owner.z-3.125)/6.25
  Require ($gx-eq[Math]::Floor($gx)-and$gz-eq[Math]::Floor($gz)-and$gx-ge0-and$gx-lt32-and$gz-ge0-and$gz-lt32-and!$cells.ContainsKey($key)) 'Duplicate/non-native/out-of-owner centre.'
  Require ($p.valid-and$p.ok-and$p.revision-eq$d.before.field.revision) 'Stale/invalid centre query.'
  $head=Finite $p.height;$depth=Finite $p.depth;$bed=Finite $p.terrainY
  $nativeBed=($vertices[(Key ($x+3.125) ($z-3.125))]+$vertices[(Key ($x-3.125) ($z+3.125))])*.5
  $tolerance=1e-5+[Math]::Max([Math]::Abs($head),[Math]::Abs($bed))*[Math]::Pow(2,-22)
  Require ($depth-ge0-and[Math]::Abs($bed-$nativeBed)-le$tolerance-and
   [Math]::Abs($depth-[Math]::Max(0.0,$head-$nativeBed))-le$tolerance) ("Reported depth/bed/head violates actual native triangle support at ${key}: head=$head depth=$depth terrainY=$bed nativeBed=$nativeBed tolerance=$tolerance")
  $cells[$key]=@{key=$key;x=$x;z=$z;head=$head;depth=$depth;bed=$nativeBed;flowX=(Finite $p.flowX);flowZ=(Finite $p.flowZ)}
 }
 $edges=Edges $cells $vertices
 return @{path=(Resolve-Path -LiteralPath $path).Path;sha256=(Get-FileHash -LiteralPath $path).Hash;
  nativeSha256=$descriptor.sha256;nativeGeometrySha256=(GeometryHash $vertices);minimumNativeBed=($vertices.Values|Measure-Object -Minimum).Minimum;
  world=$native.source.world;sourceWrp=$native.source.wrpSha256;seaLevel=$native.header.seaLevel;range=2048;spacing=6.25;
  processWorldToken=$arm.gates.wetFine.worldToken;
  stamp=$r.installed.stamp;state=$d.before;owner=$d.owner;cells=$cells;vertices=$vertices;edges=$edges;summary=(Summary $cells $edges)}
}
function Match($a,$b){
 Require ($a.world-ceq$b.world-and$a.sourceWrp-ceq$b.sourceWrp-and$a.range-eq$b.range-and$a.spacing-eq$b.spacing) 'Different original map/domain source.'
 $commonVertices=0;foreach($key in $a.vertices.Keys){if(!$b.vertices.ContainsKey($key)){continue};++$commonVertices;Require ($a.vertices[$key]-eq$b.vertices[$key]) 'Same coordinate has changed native vertex.'}
 $common=0;$becameDry=0;$becameWet=0
 foreach($key in $a.cells.Keys){$old=$a.cells[$key];$new=$b.cells[$key];if($null-eq$new){continue};++$common
  Require ($old.bed-eq$new.bed) 'Same coordinate has changed native bed.'
  if($old.depth-gt.001-and$new.depth-le.001){++$becameDry};if($old.depth-le.001-and$new.depth-gt.001){++$becameWet}
 }
 $sameSea=$a.seaLevel-eq$b.seaLevel
 $higherSea=[Math]::Max([double]$a.seaLevel,[double]$b.seaLevel)
 $minimumBed=[Math]::Min([double]$a.minimumNativeBed,[double]$b.minimumNativeBed)
 if(!$sameSea){
  Require ($common-eq$a.cells.Count-and$common-eq$b.cells.Count-and$common-gt0-and
   $commonVertices-eq$a.vertices.Count-and$commonVertices-eq$b.vertices.Count-and
   $a.nativeGeometrySha256-ceq$b.nativeGeometrySha256) 'Unequal sea requires the same complete native owner geometry.'
  Require ($minimumBed-gt$higherSea+.02) 'Unequal sea could affect coastal/submerged native support.'
 }
 $bEdges=@{};foreach($edge in $b.edges){$bEdges[$edge.key]=$edge}
 $matched=[Collections.Generic.List[object]]::new()
 foreach($edge in $a.edges){$new=$bEdges[$edge.key];if($null-eq$new){continue}
  Require ($edge.face0-eq$new.face0-and$edge.face1-eq$new.face1) 'Matched face geometry changed.'
  if($edge.bothWet-and$new.bothWet){$matched.Add(@{x=$edge.x;z=$edge.z;axis=$edge.axis;baseline=$edge.delta;candidate=$new.delta;fullyWetBoth=$edge.fullyWet-and$new.fullyWet})}
 }
 return @{commonNativeCentres=$common;commonNativeVertices=$commonVertices;becameDry=$becameDry;becameWet=$becameWet;
  sea=@{equal=$sameSea;baseline=$a.seaLevel;candidate=$b.seaLevel;minimumNativeBed=$minimumBed;clearanceAboveBoth=$minimumBed-$higherSea;
   scope=if($sameSea){'Equal recorded sea control'}else{'Same complete inland geometry above both actual sea thresholds; observations only, globally noncausal'}};
  matchedWetPairBaseline=(Distribution @($matched|ForEach-Object baseline));matchedWetPairCandidate=(Distribution @($matched|ForEach-Object candidate));
  matchedFullyWetBaseline=(Distribution @($matched|Where-Object fullyWetBoth|ForEach-Object baseline));
  matchedFullyWetCandidate=(Distribution @($matched|Where-Object fullyWetBoth|ForEach-Object candidate));pairs=$matched.ToArray()}
}
if($SelfTest){
 $cells=@{};$vertices=@{}
 for($z=0;$z-le2;++$z){for($x=0;$x-le2;++$x){$vertices[(Key ($x*6.25) ($z*6.25))]=10.0}}
 for($z=0;$z-lt2;++$z){for($x=0;$x-lt2;++$x){$px=($x+.5)*6.25;$pz=($z+.5)*6.25;$key=Key $px $pz;$h=12+((($x+$z)%2)*2-1)*.04;$cells[$key]=@{key=$key;x=$px;z=$pz;head=$h;depth=$h-10;bed=10;flowX=0;flowZ=0}}}
 $edges=Edges $cells $vertices;$summary=Summary $cells $edges
 Require ($summary.wetEdges.count-eq4-and[Math]::Abs($summary.wetEdges.mean-.08)-lt1e-10-and$summary.fullyWetNativeFaces.count-eq4) 'Checker observation self-test failed.'
 $geometryHash=GeometryHash $vertices
 $a=@{world='noe';sourceWrp='same';range=2048;spacing=6.25;seaLevel=0;cells=$cells;vertices=$vertices;nativeGeometrySha256=$geometryHash;minimumNativeBed=10;edges=$edges}
 $b=@{world='noe';sourceWrp='same';range=2048;spacing=6.25;seaLevel=0;cells=@{};vertices=$vertices.Clone();nativeGeometrySha256=$geometryHash;minimumNativeBed=10;edges=@()}
 Require ((Match $a $b).commonNativeCentres-eq0) 'Different owner falsely matched.'
 foreach($p in $cells.Values){$b.cells[$p.key]=$p.Clone();$b.cells[$p.key].head=12;$b.cells[$p.key].depth=2};$b.edges=Edges $b.cells $vertices
 $m=Match $a $b;Require ($m.matchedWetPairBaseline.count-eq4-and$m.matchedWetPairCandidate.maximum-eq0) 'Matched head observations failed.'
 $b.seaLevel=.15;$m=Match $a $b;Require (!$m.sea.equal-and$m.sea.clearanceAboveBoth-gt9) 'Legitimate inland fresh-session tide rejected.'
 foreach($change in @(@('world','eden'),@('sourceWrp','different'),@('spacing',12.5),@('range',1024),@('minimumNativeBed',.16),@('nativeGeometrySha256','different'))){
  $saved=$b[$change[0]];$b[$change[0]]=$change[1];$caught=$false;try{$null=Match $a $b}catch{$caught=$true};Require $caught ('Invalid cross-run control accepted: '+$change[0]);$b[$change[0]]=$saved
 }
 $vertex=@($b.vertices.Keys)[0];$b.vertices[$vertex]=10.01;$caught=$false;try{$null=Match $a $b}catch{$caught=$true};Require $caught 'Changed outer/native vertex accepted.';$b.vertices[$vertex]=10
 SeaSource .15 .15;$caught=$false;try{SeaSource .15 .16}catch{$caught=$true};Require $caught 'Within-run mismatched sea accepted.'
 $first=@($b.cells.Keys)[0];$b.cells[$first].bed=10.01;$caught=$false;try{$null=Match $a $b}catch{$caught=$true};Require $caught 'Changed source bed was accepted.'
 Write-Host 'Native-head observation self-test PASS; no game or compiler used.';return
}
Require ([bool]$Baseline-and[bool]$Candidate) 'Provide both completed campaign result paths.'
$old=ReadSnapshot $Baseline;$new=ReadSnapshot $Candidate;$match=Match $old $new
$status=if($match.matchedWetPairBaseline.count-eq0){'observations-only-no-common-wet-pairs'}elseif(!$match.sea.equal){'matched-native-observations-unequal-sea-noncausal'}else{'matched-native-observations'}
function PublicSnapshot($s){return @{path=$s.path;sha256=$s.sha256;nativeSha256=$s.nativeSha256;nativeGeometrySha256=$s.nativeGeometrySha256;processWorldToken=$s.processWorldToken;seaLevel=$s.seaLevel;stamp=$s.stamp;state=$s.state;owner=$s.owner;summary=$s.summary}}
$report=[ordered]@{status=$status;map=$Map;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
 baseline=(PublicSnapshot $old);candidate=(PublicSnapshot $new);matched=$match;
 sameInput=($old.sha256-ceq$new.sha256);
 limitations='Descriptive paused centre-grid observations only. Different rain history, volume, incoming flow or illumination prevent causal before/after acceptance. No temporal oscillation proof, renderer coverage/normal proof, visual acceptance, mass seeding, source change, or cosmetic smoothing.'}
if($Output){$report|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $Output -Encoding utf8}
[pscustomobject]@{status=$status;baselineWetPairs=$old.summary.wetEdges.count;candidateWetPairs=$new.summary.wetEdges.count;
 baselineMeanStepM=$old.summary.wetEdges.mean;candidateMeanStepM=$new.summary.wetEdges.mean;
 baselineMaxStepM=$old.summary.wetEdges.maximum;candidateMaxStepM=$new.summary.wetEdges.maximum;
 commonCentres=$match.commonNativeCentres;matchedWetPairs=$match.matchedWetPairBaseline.count;sameInput=$report.sameInput;output=$Output}|ConvertTo-Json
