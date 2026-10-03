[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$ResultJson,[string]$Map='eden',
 [string]$ViewName='contact-overcast-low',[Parameter(Mandatory=$true)][string]$OutputCsv)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Finite($v){$n=[double]$v;Require ($null-ne $v -and ![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Missing/nonfinite actual receipt field.';return $n}
$result=Get-Content -LiteralPath $ResultJson -Raw|ConvertFrom-Json
Require ($result.status-ceq 'functional-available-sources-passed-explicit-soft-gaps-visual-review-pending' -and $result.maps.$Map.lifecycle.status-ceq 'ok' -and $result.maps.$Map.lifecycle.exit_code-eq 0) 'Only a completed passing installed runtime receipt may be exported.'
$q=$result.maps.$Map.gates.mudLighting.views.$ViewName.fragmentDiagnostic.waterReceipt.edgeSnapshot
Require ($null-ne $q -and $q.side-eq 17 -and $q.spacing-eq .125 -and $q.points.Count-eq 289) 'Complete actual paused edge snapshot required.'
Require ($q.before.time-eq $q.after.time -and $q.before.mud.revision-eq $q.after.mud.revision -and $q.before.mud.wetness-eq $q.after.mud.wetness) 'Physical store changed during edge snapshot.'
$wet=Finite $q.terrainWetness.lastQueuedRow.queuedPad1
Require ($wet-ge 0 -and $wet-le 1) 'Actual queued terrain wetness outside range.'
$eye=@($q.camera.position);Require ($eye.Count-eq 3) 'Actual camera unavailable.'
$rows=@('# x,y,z,physical_dx,physical_dz,native_supported_depth,queued_wetness,cpu_source_eligible,eye_x,eye_y,eye_z')
$identity=$q.points[0].identity|ConvertTo-Json -Compress -Depth 12;$index=0
foreach($point in $q.points){
 Require (($point.identity|ConvertTo-Json -Compress -Depth 12)-ceq $identity) 'Native water/source identity or budget differs inside paused grid.'
 $expectedX=[double][single]([double]$q.centre[0]+(($index%17)-8)*.125)
 $expectedZ=[double][single]([double]$q.centre[1]+([Math]::Floor($index/17)-8)*.125)
 Require ($point.actualSourcePoint.x-eq $expectedX -and $point.actualSourcePoint.z-eq $expectedZ -and $point.requested.x-eq $expectedX -and $point.requested.z-eq $expectedZ) 'Actual edge coordinates/order differ from bounded requested grid.'
 ++$index

 $p=$point.actualSourcePoint;$d=$point.depths;Require ($null-ne $p.sourceEligible) 'Actual CPU source admission unavailable.'
 $values=@((Finite $p.x),(Finite $p.surfaceY),(Finite $p.z),(Finite $p.surfaceDx),(Finite $p.surfaceDz),
  (Finite $d.projectedNativeDepth),$wet,[int][bool]$p.sourceEligible,(Finite $eye[0]),(Finite $eye[1]),(Finite $eye[2]))
 Require ($values[5]-ge 0) 'Negative physical support depth.'
 $rows+=,(($values|ForEach-Object{([double]$_).ToString('R',$culture)}) -join ',')
}
$target=[IO.Path]::GetFullPath($OutputCsv);[IO.File]::WriteAllLines($target,$rows)
$sidecar=@{resultJson=[IO.Path]::GetFullPath($ResultJson);resultSha256=(Get-FileHash -LiteralPath $ResultJson).Hash;
 csvSha256=(Get-FileHash -LiteralPath $target).Hash;installed=$result.installed;map=$Map;view=$ViewName;camera=$q.camera;
 before=$q.before;after=$q.after;rows=289;terrainWetness=$q.terrainWetness;
 scope='Actual CPU owner geometry/source/depth and logged queued wetness exported for GPU helper replay. CPU sourceEligible is not uploaded material bits, slopes are not fragment footprint-filtered normals, and no current-frame SSR hit or SkyUniform replay is claimed.'}
$sidecar|ConvertTo-Json -Depth 10|Set-Content -LiteralPath ($target+'.json')
Write-Host ('Actual bounded wet-soil edge probe CSV: '+$target)
