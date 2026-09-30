# Extract actual runner helpers; no installed-file/process/network/GPU access.
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$path=Join-Path $PSScriptRoot 'Test-MudFootprints.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$culture=[Globalization.CultureInfo]::InvariantCulture
foreach ($name in @('Require','Finite-MudNumber','Assert-MudState','Assert-MudSample','Assert-MudGeometry','Assert-MudPaused','Parse-MudStep','Assert-MudPair','Decode-Eval')) {
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if (!$definition) { throw "Actual helper missing: $name" };. ([scriptblock]::Create($definition.Extent.Text))
}
$script:checks=0
function Check([bool]$value) { if (!$value) { throw 'Actual helper result differs.' };++$script:checks }
function Refuses([scriptblock]$action) { $refused=$false;try { $null=& $action } catch { $refused=$true };Check $refused }
foreach ($value in @(0,1,.65,-.06,4975)) { Check ((Finite-MudNumber $value) -eq $value) }
foreach ($value in @($null,$true,'0.5',@(.5),[double]::NaN,[double]::PositiveInfinity)) { Refuses {Finite-MudNumber $value} }
function New-Sample {
    return @{enabled=$true;wetness=.65;chunks=1;revision=8;rejected=0;x=4975.03125;z=4675.03125;offset=0;mudDx=0;mudDz=0;
        surfaceY=21.5;surfaceDx=.003;surfaceDz=-.007;sourceEligible=$true;world='noe/noe.wrp';texture='o/pole2.paa'}
}
$before=New-Sample;$after=New-Sample
Assert-MudSample $before;++$script:checks
$after.offset=-.034;$after.mudDx=.04;$after.mudDz=-.08
$after.surfaceY=21.466;$after.surfaceDx=.043;$after.surfaceDz=-.087
$proof=Assert-MudGeometry $before $after;Check ([Math]::Abs($proof.depthDelta+.034) -lt 1e-9)
Check ([Math]::Abs($proof.supportDelta-$proof.depthDelta) -lt 1e-9)
Check ([Math]::Abs($proof.dxDelta-.04) -lt 1e-9 -and [Math]::Abs($proof.dzDelta+.08) -lt 1e-9)
$after.surfaceY=21.5;Refuses {Assert-MudGeometry $before $after};$after.surfaceY=21.466
$after.surfaceDx=.003;Refuses {Assert-MudGeometry $before $after};$after.surfaceDx=.043
$after.surfaceDz=-.007;Refuses {Assert-MudGeometry $before $after};$after.surfaceDz=-.087
$after.x+=.125;Refuses {Assert-MudGeometry $before $after};$after.x=$before.x
$after.texture='o/pole1.paa';Refuses {Assert-MudGeometry $before $after};$after.texture=$before.texture
foreach ($field in @('x','z','offset','mudDx','mudDz','surfaceY','surfaceDx','surfaceDz','wetness','chunks','revision','rejected')) {
    $bad=New-Sample;$bad[$field]=[double]::NaN;Refuses {Assert-MudSample $bad}
}
foreach ($case in @(@('enabled',0),@('enabled',$false),@('wetness',-.001),@('wetness',1.01),@('chunks',-1),@('revision',1.5),@('rejected',-1),@('offset',.001),@('offset',-.351),@('sourceEligible','true'),@('texture',$null))) {
    $bad=New-Sample;$bad[$case[0]]=$case[1];Refuses {Assert-MudSample $bad}
}
function New-Paused {
    return @{time=155.125;mud=(New-Sample);weather=@{rain=0;particleDensity=1;particleSnowflakes=$false;liquidRain=1};
        actor=@(4975,4678.25,21.5);heading=0;pose='civil';uniformWetness=.1}
}
$first=New-Paused;$second=New-Paused;Assert-MudPaused $first $second;++$script:checks
foreach ($field in @('wetness','chunks','revision','rejected')) { $bad=New-Paused;$bad.mud[$field]+=.01;Refuses {Assert-MudPaused $first $bad} }
foreach ($field in @('rain','particleDensity','liquidRain')) { $bad=New-Paused;$bad.weather[$field]+=.01;Refuses {Assert-MudPaused $first $bad} }
$bad=New-Paused;$bad.weather.particleSnowflakes=$true;Refuses {Assert-MudPaused $first $bad}
$bad=New-Paused;$bad.time+=.0001;Refuses {Assert-MudPaused $first $bad}
$bad=New-Paused;$bad.actor[2]-=.001;Refuses {Assert-MudPaused $first $bad}
$bad=New-Paused;$bad.pose='Walk';Refuses {Assert-MudPaused $first $bad}
$bad=New-Paused;$bad.uniformWetness+=.0001;Refuses {Assert-MudPaused $first $bad}
$line='[INFO] MUD_STEP x=4975.123 z=4675.234 surface=Default files=default sound=normalExt texture=o\pole2.paa wet=0.6500 depth=0.0379 revision=8 chunks=1'
$row=Parse-MudStep $line;Check ($row.x -eq 4975.123 -and $row.texture -ceq 'o\pole2.paa' -and $row.depth -eq .0379)
foreach ($bad in @('MUD_STEP synthetic',$line.Replace('depth=0.0379','depth=0.0000'),$line.Replace('depth=0.0379','depth=0.3510'),$line.Replace('wet=0.6500','wet=0.1000'),$line.Replace('chunks=1','chunks=0'))) { Refuses {Parse-MudStep $bad} }
$oldCulture=[Threading.Thread]::CurrentThread.CurrentCulture
try {
    [Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Check ((Parse-MudStep $line).wet -eq .65)
} finally { [Threading.Thread]::CurrentThread.CurrentCulture=$oldCulture }
function New-Pair {
    return @{deployedFrom='installed-test-commit';files=@(@{name='OpenPoseidon.exe';path='D:\game\OpenPoseidon.exe';sha256=('a'*64);bytes=100;writtenUtc='2026-09-30T19:00:00Z'},@{name='wgpu_renderer.dll';path='D:\game\wgpu_renderer.dll';sha256=('b'*64);bytes=200;writtenUtc='2026-09-30T19:00:00Z'})}
}
$pair=New-Pair;$same=New-Pair;Assert-MudPair $pair $same;++$script:checks
$same.deployedFrom='other';Refuses {Assert-MudPair $pair $same}
foreach ($field in @('name','path','sha256','bytes','writtenUtc')) { $changed=New-Pair;$changed.files[1][$field]='changed';Refuses {Assert-MudPair $pair $changed} }
$badPair=New-Pair;$badPair.files[0].sha256='a';Refuses {Assert-MudPair $badPair $badPair}
Check ((Decode-Eval '[4975,4675,21.5]').Count -eq 3);Check ((Decode-Eval '"OK"') -ceq 'OK')
Refuses {Decode-Eval 'UNKNOWN COMMAND'}
Write-Host "Actual mud runner helper checks passed: $checks. No game/GPU used."
