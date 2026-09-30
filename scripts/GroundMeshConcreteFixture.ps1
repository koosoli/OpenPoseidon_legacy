# Fixture-only helpers. No launch, deployment or permanent content writes.
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"

function Get-GroundMeshConcretePlan {
    param([Parameter(Mandatory)][scriptblock]$SendCommand,
          [double]$X=9486, [double]$Z=3006,
          [double]$ModelTopY=2.4916980266571045)
    foreach ($v in @($X,$Z,$ModelTopY)) {
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { throw 'Fixture unavailable: nonfinite placement.' }
    }
    $ground = Get-TerrainPuddleFixtureHeight $SendCommand $X $Z
    $top = $ground + 0.05
    $samples = @()
    # A 3m-wide core includes the aimed central ROI. This is a bounded fit
    # diagnostic, not proof of every fragment on the 20m-wide solid model.
    foreach ($dx in @(-1.5,0,1.5)) {
        foreach ($dz in @(-1.5,0,1.5)) {
            $h = Get-TerrainPuddleFixtureHeight $SendCommand ($X+$dx) ($Z+$dz)
            if ([Math]::Abs($top-$h) -gt 0.12) { throw 'Fixture unavailable: core cannot fit the actual .15m fragment ground gate.' }
            $samples += @{x=$X+$dx;z=$Z+$dz;height=$h;topDelta=$top-$h}
        }
    }
    return @{x=$X;z=$Z;ground=$ground;modelTopY=$ModelTopY;top=$top;originHeight=$top-$ModelTopY;fitSamples=$samples}
}

function Set-GroundMeshConcretePose {
    param([Parameter(Mandatory)][scriptblock]$SendCommand,
          [Parameter(Mandatory)]$Plan,[ValidateRange(0,10)][double]$Raise=0)
    if (!$env:LOCK_OWNER) { throw 'Concrete fixture requires shared game ownership.' }
    $c=[Globalization.CultureInfo]::InvariantCulture
    $expected=@([double]$Plan.x,[double]$Plan.z,([double]$Plan.originHeight+$Raise))
    $tuple=($expected | ForEach-Object {$_.ToString('R',$c)}) -join ','
    $null=& $SendCommand @{cmd='exec';code=('groundPuddleConcrete setDir 0; groundPuddleConcrete setPosASL ['+$tuple+']; groundPuddleConcrete allowDamage false')}
    $reply=& $SendCommand @{cmd='eval';code='getPosASL groundPuddleConcrete'}
    $actual=([string]$reply.result).Trim('"') | ConvertFrom-Json
    if ($actual.Count -ne 3) { throw 'Fixture unavailable: no actual concrete pose.' }
    for ($i=0;$i -lt 3;$i++) {
        $v=[double]$actual[$i]
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v) -or [Math]::Abs($v-$expected[$i]) -gt .01) {
            throw 'Fixture unavailable: actual concrete pose differs from request.'
        }
    }
    return @{actualPosition=$actual;requestedPosition=$expected;raise=$Raise;topHeight=$Plan.top+$Raise}
}

function New-GroundMeshConcreteFixture {
    param([Parameter(Mandatory)][scriptblock]$SendCommand,[Parameter(Mandatory)]$Plan)
    if (!$env:LOCK_OWNER) { throw 'Concrete fixture requires shared game ownership.' }
    $c=[Globalization.CultureInfo]::InvariantCulture
    $xz=$Plan.x.ToString('R',$c)+','+$Plan.z.ToString('R',$c)
    $null=& $SendCommand @{cmd='exec';code=('groundPuddleConcrete="GroundPuddleConcreteFixture" createVehicle ['+$xz+',0]')}
    $class=& $SendCommand @{cmd='eval';code='typeOf groundPuddleConcrete'}
    if (([string]$class.result).Trim('"') -cne 'GroundPuddleConcreteFixture') { throw 'Fixture unavailable: private class failed to mount/spawn.' }
    $pose=Set-GroundMeshConcretePose $SendCommand $Plan
    $identity=& $SendCommand @{cmd='eval';code='groundPuddleConcrete'}
    if (([string]$identity.result) -notmatch '(^|[\\/\s:])molo_beton(?:\.p3d)?(?:$|")') {
        throw 'Fixture unavailable: actual debug model is not audited stock Molo_beton.'
    }
    return @{class='GroundPuddleConcreteFixture';classAuthority='private alias, not a stock class';model='data3d/molo_beton.p3d';debugName=$identity.result;pose=$pose;plan=$Plan}
}

function Get-GroundMeshConcreteCamera {
    param([Parameter(Mandatory)]$Plan,[double]$Raise=0,[switch]$Underside)
    $c=[Globalization.CultureInfo]::InvariantCulture
    $height=if ($Underside) {$Plan.ground+1} else {$Plan.top+$Raise+1}
    $z=if ($Underside) {$Plan.z} else {$Plan.z-1.5}
    $elevation=if ($Underside) {85} else {-55}
    return ((@($Plan.x,$z,$height,0,$elevation) | ForEach-Object {([double]$_).ToString('R',$c)}) -join ' ')
}

function Remove-GroundMeshConcreteFixture {
    param([Parameter(Mandatory)][scriptblock]$SendCommand)
    if (!$env:LOCK_OWNER) { throw 'Concrete fixture requires shared game ownership.' }
    $null=& $SendCommand @{cmd='exec';code='deleteVehicle groundPuddleConcrete'}
    $reply=& $SendCommand @{cmd='eval';code='isNull groundPuddleConcrete'}
    if (([string]$reply.result).Trim('"') -ne 'true') { throw 'Concrete fixture failed to leave isolated mission.' }
}
