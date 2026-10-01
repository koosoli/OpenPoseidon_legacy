# Installed read-only final-pose/lifetime proof. Process ownership is shared with
# the accepted no-motion proof runner; no pose setter, solver or root movement.
[CmdletBinding()]
param([switch]$SelfTest,
      [ValidateRange(8,120)][int]$CaptureDeadlineSeconds=60,
      [ValidateRange(1,4)][double]$ExpiryAcceleration=4,
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='final-pose-diagnostic',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$NativeAddons='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons')
$ErrorActionPreference='Stop'
function Require([bool]$value,[string]$message) { if(!$value) { throw $message } }
function Finite($value,[string]$field) {
    Require ($null -ne $value -and $value -is [ValueType] -and $value -isnot [bool] -and
        ![double]::IsNaN([double]$value) -and ![double]::IsInfinity([double]$value)) "Nonfinite/missing numeric $field."
}
function Vector($value,[int]$count,[string]$field) {
    Require ($value -is [array] -and $value.Count -eq $count) "Invalid $field cardinality."
    foreach($v in $value) { Finite $v $field }
}
function Whole($value,[int]$min,[int]$max,[string]$field) {
    Finite $value $field
    Require ($value -ge $min -and $value -le $max -and [Math]::Floor([double]$value) -eq $value) "Invalid $field count."
}
function Assert-Measurement($reply,[string]$identity) {
    Require ($reply.ok -is [bool] -and $reply.ok) 'Missing/failed actual final-pose getter.'
    foreach($flag in @('readonly','solver','rigAdmitted','contactAdmitted')) {
        Require ($reply.$flag -is [bool]) "Missing typed $flag."
    }
    Require ($reply.readonly -and !$reply.solver -and !$reply.rigAdmitted -and !$reply.contactAdmitted) 'Diagnostic falsely admitted motion/contact/rig.'
    Require ($reply.entity -is [string] -and $reply.entity -ceq $identity -and $identity -match '^[0-9A-Fa-f]+$') 'Actual held entity identity mismatch.'
    Require ($reply.model -is [string] -and $reply.model -ieq 'data3d\mc vojakw2.p3d') 'Getter was not actual stock western model.'
    foreach($time in @('capturedMs','measuredMs')) { Whole $reply.$time 0 ([int]::MaxValue) $time }
    Require ($reply.measuredMs -ge $reply.capturedMs -and $reply.measuredMs-$reply.capturedMs -le 30000) 'Actual lease clock invalid.'
    Require ($reply.contactScope -ceq 'road-support clearance and corpse-ignored geometry center ray; not convex overlap') 'Contact scope changed.'
    Require ($reply.anchorScope -ceq 'shared source selection centroid; paired final-bone endpoints, no calibrated joint frame') 'Anchor evidence scope changed.'
    Require ($reply.matrixLayout -ceq 'row-major 3x4 model-space affine; actual post-head/aim/graphical-leg palette with dead face tail') 'Matrix consumer/layout scope changed.'
    foreach($flag in @('headIdentityFlag','gunIdentityFlag')) { Require ($reply.$flag -is [bool]) "Missing typed $flag." }
    foreach($prefix in @('head','gun','leg')) {
        $matrix=$reply.($prefix+'Correction'); Vector $matrix 12 ($prefix+'Correction')
        $error=$reply.($prefix+'IdentityError'); Finite $error ($prefix+'IdentityError')
        $actualError=0.0
        for($r=0;$r -lt 3;++$r) { for($c=0;$c -lt 4;++$c) {
            $reference=if($r -eq $c) {1.0} else {0.0}
            $actualError=[Math]::Max($actualError,[Math]::Abs($matrix[$r*4+$c]-$reference))
        } }
        Require ([Math]::Abs($error-$actualError) -le .000001) "Actual $prefix correction metric mismatch."
    }
    $stock='pchodidlo lchodidlo pprsty lprsty lholen pholen pstehno lstehno pzadek lzadek bricho zebra hrudnik krk prameno lrameno hlava pbiceps lbiceps ploket lloket roura zbran pruka lruka'.Split(' ')
    Require ($reply.bones -is [array] -and $reply.bones.Count -eq 33 -and @($reply.bones | Sort-Object -Unique).Count -eq 33) 'Incomplete stock plus synthetic runtime bone list.'
    foreach($bone in $stock) { Require (@($reply.bones | Where-Object {$_ -ceq $bone}).Count -eq 1) "Missing/duplicate source bone $bone." }
    Require ($reply.levels -is [array] -and $reply.levels.Count -ge 6 -and $reply.levels.Count -le 32) 'Incomplete/beyond-budget actual LODs.'
    $indices=@()
    foreach($level in $reply.levels) {
        Whole $level.level 0 31 'LOD'; $indices+= $level.level
        Whole $level.points 1 4096 'points'; Whole $level.compared 0 $level.points 'compared'
        Whole $level.skippedPointTails 0 $level.points 'skippedPointTails'; Whole $level.unweighted 0 $level.points 'unweighted'
        Require ($level.compared+$level.skippedPointTails -eq $level.points) 'Actual point coverage incomplete.'
        Require ($level.faceEvaluated -is [bool] -and $level.pointOnly -is [bool] -and $level.pointOnly -eq ($level.points -eq 1)) 'Missing/invalid consumer path flags.'
        Finite $level.maxPointPaletteError 'maxPointPaletteError'; Finite $level.minAbsDeterminant 'minAbsDeterminant'
        Require ($level.maxPointPaletteError -ge 0 -and $level.minAbsDeterminant -ge 0) 'Invalid final-pose measurement sign.'
        Vector $level.paletteMin 3 'paletteMin'; Vector $level.paletteMax 3 'paletteMax'
        for($axis=0;$axis -lt 3;++$axis) { Require ($level.paletteMin[$axis] -le $level.paletteMax[$axis]) 'Final palette bounds reversed.' }
        Require ($level.palette -is [array] -and $level.palette.Count -eq $reply.bones.Count) 'Incomplete final palette.'
        foreach($matrix in $level.palette) { Vector $matrix 12 'final matrix' }
    }
    Require (@($indices | Sort-Object -Unique).Count -eq $reply.levels.Count) 'Duplicate actual LODs.'
    for($i=0;$i -lt $indices.Count;++$i) { Require ($indices -contains $i) 'Missing actual LOD index.' }
    foreach($role in @('graphical0','memory','geometry','fireGeometry','viewGeometry','landContact')) {
        Require (@($reply.levels | Where-Object {$_.role.Split('+') -contains $role}).Count -eq 1) "Missing special consumer $role."
    }
    $graph=@($reply.levels | Where-Object {$_.role.Split('+') -contains 'graphical0'})[0]
    Require ($graph.faceEvaluated) 'Actual graphical synthetic face tail not evaluated.'
    Require ($reply.proxies -is [array] -and $reply.proxies.Count -gt 0) 'Actual proxy consumers absent.'
    foreach($proxy in $reply.proxies) {
        Require ($indices -contains $proxy.level -and $proxy.selection -is [string] -and $proxy.selection.Length -gt 0) 'Invalid actual proxy source.'
        Vector $proxy.matrix 12 'actual proxy matrix'
    }
    Require ($reply.hulls -is [array] -and $reply.hulls.Count -eq 14) 'Actual authored hull census incomplete.'
    $hullBones=@('pchodidlo','lchodidlo','zebra','hlava','lbiceps','lloket','pbiceps','ploket','lstehno','lholen','pstehno','pholen','zbran','roura')
    for($i=0;$i -lt 14;++$i) {
        $hull=$reply.hulls[$i]
        Require ($hull.name -ceq ('component{0:00}' -f ($i+1)) -and $hull.bone -ceq $hullBones[$i]) 'Actual hull ownership source mismatch.'
        Require ($hull.exclusiveFullWeight -is [bool] -and $hull.exclusiveFullWeight) 'Actual hull is not full-weight single-bone.'
        Whole $hull.points 20 24 'hull points'
        Require ($hull.points -eq $(if($i -eq 13) {20} else {24})) 'Actual stock hull vertex count mismatch.'
        Vector $hull.worldMin 3 'worldMin'; Vector $hull.worldMax 3 'worldMax'
        for($axis=0;$axis -lt 3;++$axis) { Require ($hull.worldMin[$axis] -le $hull.worldMax[$axis]) 'Hull bounds reversed.' }
        Finite $hull.minRoadSupportClearance 'minRoadSupportClearance'
        Whole $hull.externalCenterRayHits 0 ([int]::MaxValue) 'externalCenterRayHits'
    }
    Require ($reply.anchors -is [array] -and $reply.anchors.Count -eq 9) 'Actual candidate anchor census incomplete.'
    $pairs=@('krk hlava','prameno pbiceps','lrameno lbiceps','pbiceps ploket','lbiceps lloket','pzadek pstehno','lzadek lstehno','pstehno pholen','lstehno lholen')
    for($i=0;$i -lt 9;++$i) {
        $anchor=$reply.anchors[$i]; Require (($anchor.first+' '+$anchor.second) -ceq $pairs[$i]) 'Anchor candidate source mismatch.'
        Whole $anchor.sharedVertices 0 4096 'sharedVertices'; Finite $anchor.separation 'separation'
        Require ($anchor.separation -ge 0) 'Negative paired-anchor distance.'
        foreach($field in @('modelAnchor','firstWorld','secondWorld')) { Vector $anchor.$field 3 $field }
        $distanceSquared=0.0
        for($axis=0;$axis -lt 3;++$axis) {$delta=$anchor.firstWorld[$axis]-$anchor.secondWorld[$axis];$distanceSquared+=$delta*$delta}
        Require ([Math]::Abs([Math]::Sqrt($distanceSquared)-$anchor.separation) -le .00001) 'Actual paired anchor endpoints/distance disagree.'
    }
    # Anchor separation, penetration and point discrepancy are observations. A
    # diagnostic pass never upgrades them to complete joint/contact admission.
}
function Assert-Refusal($reply) {
    Require ($reply.ok -is [bool] -and !$reply.ok -and $reply.error -ceq 'no-valid-hold') 'Missing hold did not return its exact real refusal.'
}
if($SelfTest) {
    $tokens=$null;$parseErrors=$null
    $hostAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-CorpsePoseProof.ps1'),[ref]$tokens,[ref]$parseErrors)
    Require ($parseErrors.Count -eq 0) 'Callback host runner has PowerShell parse errors.'
    $baselineFunction=$hostAst.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Test-ExactPausedBaseline'
    },$true)
    Require ($null -ne $baselineFunction) 'Actual baseline policy missing.'
    . ([scriptblock]::Create($baselineFunction.Extent.Text))
    $a=@{sha256='A';simulationTime=17.374};$b=@{sha256='B';simulationTime=17.374}
    Require (!(Test-ExactPausedBaseline @())) 'Empty baseline accepted.'
    Require (!(Test-ExactPausedBaseline @($a,$a))) 'Only two exact captures accepted.'
    Require (!(Test-ExactPausedBaseline @($a,$b,$a,$b,$a,$b,$a,$b))) 'Eight drifting captures accepted.'
    Require (Test-ExactPausedBaseline @($a,$b,$b,$b)) 'Three exact final baseline captures refused.'
    Require (!(Test-ExactPausedBaseline @($a,$a,$a,$b))) 'Latest drift ignored after earlier stability.'
    $refused=$false;try {Test-ExactPausedBaseline @($a,$a,@{sha256='A';simulationTime=17.375})} catch {$refused=$true}
    Require $refused 'Baseline simulation advancement accepted.'
    $identity=@(1,0,0,0,0,1,0,0,0,0,1,0)
    $mock=[ordered]@{ok=$true;readonly=$true;solver=$false;rigAdmitted=$false;contactAdmitted=$false;entity='ABCD';model='data3d\mc vojakw2.p3d';
        capturedMs=1000;measuredMs=1000;headIdentityFlag=$true;gunIdentityFlag=$true;
        headCorrection=$identity;gunCorrection=$identity;legCorrection=$identity;headIdentityError=0;gunIdentityError=0;legIdentityError=0;
        contactScope='road-support clearance and corpse-ignored geometry center ray; not convex overlap';
        anchorScope='shared source selection centroid; paired final-bone endpoints, no calibrated joint frame';
        matrixLayout='row-major 3x4 model-space affine; actual post-head/aim/graphical-leg palette with dead face tail';
        bones=@('pchodidlo lchodidlo pprsty lprsty lholen pholen pstehno lstehno pzadek lzadek bricho zebra hrudnik krk prameno lrameno hlava pbiceps lbiceps ploket lloket roura zbran pruka lruka'.Split(' '))+@('f0','f1','f2','f3','f4','f5','f6','f7');
        levels=@();proxies=@(@{level=0;selection='proxy:test';matrix=$identity});hulls=@();anchors=@()}
    $roles=@('graphical0','memory','geometry','fireGeometry','viewGeometry','landContact')
    foreach($i in 0..5) {
        $palette=@();foreach($bone in 0..32) {$palette+=,@($identity)}
        $mock.levels+=@{level=$i;role=$roles[$i];points=1;compared=1;skippedPointTails=0;unweighted=1;faceEvaluated=($i -eq 0);pointOnly=$true;
            maxPointPaletteError=0;minAbsDeterminant=1;paletteMin=@(0,0,0);paletteMax=@(1,1,1);palette=$palette}
    }
    $bones=@('pchodidlo','lchodidlo','zebra','hlava','lbiceps','lloket','pbiceps','ploket','lstehno','lholen','pstehno','pholen','zbran','roura')
    foreach($i in 0..13) {$mock.hulls+=@{name=('component{0:00}' -f ($i+1));bone=$bones[$i];points=$(if($i -eq 13) {20} else {24});
        exclusiveFullWeight=$true;worldMin=@(0,0,0);worldMax=@(1,1,1);minRoadSupportClearance=-.17;externalCenterRayHits=1}}
    foreach($pair in @('krk hlava','prameno pbiceps','lrameno lbiceps','pbiceps ploket','lbiceps lloket','pzadek pstehno','lzadek lstehno','pstehno pholen','lstehno lholen')) {
        $parts=$pair.Split(' ');$mock.anchors+=@{first=$parts[0];second=$parts[1];sharedVertices=9;separation=.12;modelAnchor=@(0,0,0);firstWorld=@(0,0,0);secondWorld=@(.12,0,0)}
    }
    $json=$mock | ConvertTo-Json -Depth 16
    Assert-Measurement (ConvertFrom-Json -InputObject $json) 'ABCD'
    foreach($bad in @('owner','admission','matrix','coverage','hull','face','source','expiry')) {
        $mutated=ConvertFrom-Json -InputObject $json
        switch($bad) {
            'owner' {$mutated.entity='DEAD'}
            'admission' {$mutated.rigAdmitted=$true}
            'matrix' {$mutated.levels[0].palette[0][3]=[double]::NaN}
            'coverage' {$mutated.levels[0].compared=0}
            'hull' {$mutated.hulls[4].bone='zebra'}
            'face' {$mutated.levels[0].faceEvaluated=$false}
            'source' {$mutated.model='data3d\other.p3d'}
            'expiry' {$mutated.measuredMs=31001}
        }
        $refused=$false;try {Assert-Measurement $mutated 'ABCD'} catch {$refused=$true};Require $refused "Bad $bad measurement accepted."
    }
    Assert-Refusal ([pscustomobject]@{ok=$false;error='no-valid-hold'})
    foreach($bad in @([pscustomobject]@{ok=$false;error='unknown command'},[pscustomobject]@{ok=$true;error='no-valid-hold'})) {
        $refused=$false; try {Assert-Refusal $bad} catch {$refused=$true}; Require $refused 'Bad refusal accepted.'
    }
    foreach($bad in @($null,[double]::NaN,[double]::PositiveInfinity,'1',$true)) {
        $refused=$false; try {Finite $bad 'mock'} catch {$refused=$true}; Require $refused 'Malformed finite metric accepted.'
    }
    $refused=$false; try {Vector @(1,2) 3 'mock'} catch {$refused=$true}; Require $refused 'Missing vector lane accepted.'
    $refused=$false; try {Assert-Measurement ([pscustomobject]@{ok=$false}) 'ABCD'} catch {$refused=$true}; Require $refused 'Absent getter accepted.'
    Write-Host 'PASS: complete typed diagnostic grammar, invalid identity/source/matrix/coverage/hull/face/expiry/admission and exact refusal gates (no game).'
    return
}
$script:pausedMeasurement=$null
$probe={param([string]$phase,[bool]$held,[string]$identity,[scriptblock]$sender)
    $reply=& $sender @{cmd='corpse_pose_state'}
    if($held) {
        Assert-Measurement $reply $identity
        if($phase -eq 'activeHold') {
            Require ($reply.capturedMs -eq $reply.measuredMs) 'Paused fresh hold clock advanced.'
            $script:pausedMeasurement=$reply | ConvertTo-Json -Depth 16 -Compress
        }
        if($phase -eq 'activePausedRepeat') {
            Require (($reply | ConvertTo-Json -Depth 16 -Compress) -ceq $script:pausedMeasurement) 'Read-only paused getter changed actual final pose/state.'
        }
    } else { Assert-Refusal $reply }
    return @{passed=$true;held=$held;actual=$reply;admission='measurement only; rig and contact remain unadmitted'}
}
$extraKeys=@('POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','WGR_OBJECT_SNOW')
$saved=@{};foreach($key in $extraKeys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try {
    $env:POSEIDON_SNOW_TEST_DEPTH='0';$env:POSEIDON_SNOWLINE='off';$env:WGR_OBJECT_SNOW='0'
    foreach($key in @('POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES')) {Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    & (Join-Path $PSScriptRoot 'Test-CorpsePoseProof.ps1') -CaptureDeadlineSeconds $CaptureDeadlineSeconds -ExpiryAcceleration $ExpiryAcceleration `
        -Label $Label -GameDir $GameDir -NativeAddons $NativeAddons -FinalPoseProbe $probe -FinalPoseProbeSource $PSCommandPath
} finally {
    foreach($key in $extraKeys) {
        if($null -eq $saved[$key]) {Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
        else {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    }
}
