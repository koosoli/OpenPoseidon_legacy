# Extract actual runner helpers; no installed-file/process/network/GPU access.
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$path=Join-Path $PSScriptRoot 'Test-SandFootprints.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$culture=[Globalization.CultureInfo]::InvariantCulture
foreach ($name in @('Require','Finite-SandNumber','Assert-SandState','Assert-SandSample','Assert-SandGeometry','Assert-SandPaused','Parse-SandStep','Assert-SandDry','Assert-SandEvidence','Assert-SandWalkPosition','Assert-SandPair','Decode-Eval')) {
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if (!$definition) { throw "Actual helper missing: $name" };. ([scriptblock]::Create($definition.Extent.Text))
}
$script:checks=0
function Check([bool]$value) { if (!$value) { throw 'Actual helper result differs.' };++$script:checks }
function Refuses([scriptblock]$action) { $refused=$false;try { $null=& $action } catch { $refused=$true };Check $refused }
foreach ($value in @(0,1,.65,-.06,2675)) { Check ((Finite-SandNumber $value) -eq $value) }
foreach ($value in @($null,$true,'0.5',@(.5),[double]::NaN,[double]::PositiveInfinity)) { Refuses {Finite-SandNumber $value} }
function New-Sample {
    return @{enabled=$true;wetness=.65;chunks=1;revision=8;rejected=0;x=2675.03125;z=5125.03125;offset=0;sandDx=0;sandDz=0;
        surfaceY=21.5;surfaceDx=.003;surfaceDz=-.007;sourceEligible=$true;world='noe/noe.wrp';texture='o/pt.paa'}
}
$before=New-Sample;$after=New-Sample
Assert-SandSample $before;++$script:checks
$after.offset=-.034;$after.sandDx=.04;$after.sandDz=-.08
$after.surfaceY=21.466;$after.surfaceDx=.043;$after.surfaceDz=-.087
$proof=Assert-SandGeometry $before $after;Check ([Math]::Abs($proof.depthDelta+.034) -lt 1e-9)
Check ([Math]::Abs($proof.supportDelta-$proof.depthDelta) -lt 1e-9)
Check ([Math]::Abs($proof.dxDelta-.04) -lt 1e-9 -and [Math]::Abs($proof.dzDelta+.08) -lt 1e-9)
$after.surfaceY=21.5;Refuses {Assert-SandGeometry $before $after};$after.surfaceY=21.466
$after.surfaceDx=.003;Refuses {Assert-SandGeometry $before $after};$after.surfaceDx=.043
$after.surfaceDz=-.007;Refuses {Assert-SandGeometry $before $after};$after.surfaceDz=-.087
$after.x+=.125;Refuses {Assert-SandGeometry $before $after};$after.x=$before.x
$after.texture='o/pole1.paa';Refuses {Assert-SandGeometry $before $after};$after.texture=$before.texture
foreach ($field in @('x','z','offset','sandDx','sandDz','surfaceY','surfaceDx','surfaceDz','wetness','chunks','revision','rejected')) {
    $bad=New-Sample;$bad[$field]=[double]::NaN;Refuses {Assert-SandSample $bad}
}
foreach ($case in @(@('enabled',0),@('enabled',$false),@('wetness',-.001),@('wetness',1.01),@('chunks',-1),@('revision',1.5),@('rejected',-1),@('offset',.021),@('offset',-.151),@('sourceEligible','true'),@('texture',$null))) {
    $bad=New-Sample;$bad[$case[0]]=$case[1];Refuses {Assert-SandSample $bad}
}
function New-Paused {
    return @{time=155.125;sand=(New-Sample);weather=@{rain=0;particleDensity=1;particleSnowflakes=$false;liquidRain=1};
        actor=@(2675,5129.25,21.5);heading=0;pose='civil';uniformWetness=.1}
}
$first=New-Paused;$second=New-Paused;Assert-SandPaused $first $second;++$script:checks
foreach ($field in @('wetness','chunks','revision','rejected')) { $bad=New-Paused;$bad.sand[$field]+=.01;Refuses {Assert-SandPaused $first $bad} }
foreach ($field in @('rain','particleDensity','liquidRain')) { $bad=New-Paused;$bad.weather[$field]+=.01;Refuses {Assert-SandPaused $first $bad} }
$bad=New-Paused;$bad.weather.particleSnowflakes=$true;Refuses {Assert-SandPaused $first $bad}
$bad=New-Paused;$bad.time+=.0001;Refuses {Assert-SandPaused $first $bad}
$bad=New-Paused;$bad.actor[2]-=.001;Refuses {Assert-SandPaused $first $bad}
$bad=New-Paused;$bad.pose='Walk';Refuses {Assert-SandPaused $first $bad}
$bad=New-Paused;$bad.uniformWetness+=.0001;Refuses {Assert-SandPaused $first $bad}
$line='[INFO] SAND_STEP x=2675.123 z=5125.234 surface=SandDark files=pt?????? sound=sand texture=o\pt.paa wet=0.0000 depth=0.1000 rim=0.0140 revision=8 chunks=1'
$row=Parse-SandStep $line;Check ($row.x -eq 2675.123 -and $row.texture -ceq 'o\pt.paa' -and $row.depth -eq .10 -and $row.rim -eq .014 -and $row.wet -eq 0)
foreach ($bad in @('SAND_STEP synthetic',$line.Replace('depth=0.1000','depth=0.0000'),$line.Replace('depth=0.1000','depth=0.1510'),$line.Replace('wet=0.0000','wet=-0.001'),$line.Replace('rim=0.0140','rim=0.0210'),$line.Replace('rim=0.0140','rim=0.0000'),$line.Replace('chunks=1','chunks=0'))) { Refuses {Parse-SandStep $bad} }
$oldCulture=[Threading.Thread]::CurrentThread.CurrentCulture
try {
    [Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Check ((Parse-SandStep $line).depth -eq .10)
} finally { [Threading.Thread]::CurrentThread.CurrentCulture=$oldCulture }
function New-Pair {
    return @{deployedFrom='installed-test-commit';files=@(@{name='OpenPoseidon.exe';path='D:\game\OpenPoseidon.exe';sha256=('a'*64);bytes=100;writtenUtc='2026-09-30T19:00:00Z'},@{name='wgpu_renderer.dll';path='D:\game\wgpu_renderer.dll';sha256=('b'*64);bytes=200;writtenUtc='2026-09-30T19:00:00Z'})}
}
$pair=New-Pair;$same=New-Pair;Assert-SandPair $pair $same;++$script:checks
$same.deployedFrom='other';Refuses {Assert-SandPair $pair $same}
foreach ($field in @('name','path','sha256','bytes','writtenUtc')) { $changed=New-Pair;$changed.files[1][$field]='changed';Refuses {Assert-SandPair $pair $changed} }
$badPair=New-Pair;$badPair.files[0].sha256='a';Refuses {Assert-SandPair $badPair $badPair}
Check ((Decode-Eval '[2675,5125,21.5]').Count -eq 3);Check ((Decode-Eval '"OK"') -ceq 'OK')
Refuses {Decode-Eval 'UNKNOWN COMMAND'}
$dry=@{rain=0;particleDensity=0;particleSnowflakes=$false;liquidRain=0}
Assert-SandDry $dry;++$script:checks
foreach ($field in @('rain','particleDensity','liquidRain')) { $bad=$dry.Clone();$bad[$field]=.02;Refuses {Assert-SandDry $bad};$bad[$field]=[double]::NaN;Refuses {Assert-SandDry $bad} }
$bad=$dry.Clone();$bad.particleSnowflakes=$true;Refuses {Assert-SandDry $bad}
$rim=New-Sample;$rim.offset=.014;$rim.surfaceY+=.014;$rim.sandDx=.04;$rim.surfaceDx+=.04
$rimProof=Assert-SandGeometry $before $rim
Check ([Math]::Abs($rimProof.supportDelta-.014) -lt 1e-9)
$rim.surfaceY-=.014;Refuses {Assert-SandGeometry $before $rim}
$evidence=Assert-SandEvidence @($proof,$rimProof)
Check ($evidence.negativeCount -eq 1 -and $evidence.positiveCount -eq 1)
Refuses {Assert-SandEvidence @($proof)};Refuses {Assert-SandEvidence @($rimProof)}
$flat=$proof.Clone();$flat.dxDelta=0;$flat.dzDelta=0;Refuses {Assert-SandEvidence @($flat,$rimProof)}
$shallow=$proof.Clone();$shallow.depthDelta=-.029;Refuses {Assert-SandEvidence @($shallow,$rimProof)}
$lowRim=$rimProof.Clone();$lowRim.depthDelta=.001;Refuses {Assert-SandEvidence @($proof,$lowRim)}
foreach ($position in @(@(2675,5125,14.9836),@(2678.44,5128.62,14.98),@(2674,5125,14.98),@(2679.4,5131.6,14.98))) {
    Assert-SandWalkPosition $position;++$script:checks
}
foreach ($position in @(@(2673.99,5125,14.98),@(2679.41,5125,14.98),@(2675,5124.99,14.98),@(2675,5131.61,14.98),
    @([double]::NaN,5125,14.98),@(2675,5125,$true),@(2675,5125))) { Refuses {Assert-SandWalkPosition $position} }
Refuses {Assert-SandWalkPosition $null}
Write-Host "Actual sand runner helper checks passed: $checks. No game/GPU used."
