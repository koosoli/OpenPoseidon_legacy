# CPU-only actual fixture helper tests. No game, GPU, deployment or file writes.
$ErrorActionPreference='Stop'
. "$PSScriptRoot/GroundMeshConcreteFixture.ps1"
$oldOwner=$env:LOCK_OWNER
$env:LOCK_OWNER='cpu-fixture-test'
$script:queries=@();$script:position=@();$script:badHeight=$false;$script:badPosition=$false;$script:badModel=$false
function Assert($condition,[string]$message) {if (!$condition) {throw $message}}
function Refuses([scriptblock]$action,[string]$pattern) {
    $caught=$false
    try {& $action | Out-Null} catch {
        $caught=$true
        Assert ($_.Exception.Message -like $pattern) ('Unexpected refusal: '+$_.Exception.Message)
    }
    Assert $caught ('Did not refuse '+$pattern)
}
$mock={param($q)
    if ($q.cmd -eq 'water_bathymetry') {
        $script:queries+=,@($q.x,$q.z)
        $height=if ($script:badHeight -and $q.x -ne 9486) {100.3} else {100.0}
        return @{samples=,@([double]$q.x,[double]$q.z,$height)}
    }
    if ($q.cmd -eq 'exec') {
        if ($q.code -match 'setPosASL \[([^]]+)\]') {
            $script:position=@($Matches[1] -split ',' | ForEach-Object {[double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture)})
        }
        return @{ok=$true}
    }
    if ($q.code -eq 'typeOf groundPuddleConcrete') {return @{result='"GroundPuddleConcreteFixture"'}}
    if ($q.code -eq 'getPosASL groundPuddleConcrete') {
        $pos=$script:position.Clone()
        if ($script:badPosition) {$pos[2]+=0.1}
        return @{result=($pos | ConvertTo-Json -Compress)}
    }
    if ($q.code -eq 'groundPuddleConcrete') {return @{result=$(if ($script:badModel) {'"42: data3d/Heli_H"'} else {'"42: data3d/Molo_beton"'})}}
    if ($q.code -eq 'isNull groundPuddleConcrete') {return @{result='true'}}
    throw ('Unexpected mock request '+($q | ConvertTo-Json -Compress))
}
try {
    $plan=Get-GroundMeshConcretePlan -SendCommand $mock
    Assert ($script:queries.Count -eq 10) 'Did not inspect actual centre plus nine fit samples.'
    Assert ([Math]::Abs($plan.top-100.05) -lt 1e-9) 'Wrong top seating clearance.'
    Assert ([Math]::Abs($plan.originHeight+$plan.modelTopY-$plan.top) -lt 1e-9) 'Top/model origin mismatch.'
    $fixture=New-GroundMeshConcreteFixture -SendCommand $mock -Plan $plan
    Assert ($fixture.classAuthority -eq 'private alias, not a stock class') 'Alias misrepresented.'
    $pose=Set-GroundMeshConcretePose -SendCommand $mock -Plan $plan -Raise 1
    Assert ([Math]::Abs($pose.topHeight-101.05) -lt 1e-9) 'Raised control is not one metre above seated top.'
    Assert ((Get-GroundMeshConcreteCamera $plan -Raise 1) -eq '9486 3004.5 102.05 0 -55') 'Raised camera changes relative surface framing.'
    Assert ((Get-GroundMeshConcreteCamera $plan -Underside) -eq '9486 3006 101 0 85') 'Underside camera is not below raised solid.'
    Remove-GroundMeshConcreteFixture -SendCommand $mock
    $script:badHeight=$true
    Refuses {Get-GroundMeshConcretePlan -SendCommand $mock} 'Fixture unavailable: core cannot fit*'
    $script:badHeight=$false;$script:badPosition=$true
    Refuses {Set-GroundMeshConcretePose -SendCommand $mock -Plan $plan} 'Fixture unavailable: actual concrete pose*'
    $script:badPosition=$false;$script:badModel=$true
    Refuses {New-GroundMeshConcreteFixture -SendCommand $mock -Plan $plan} 'Fixture unavailable: actual debug model*'
    $env:LOCK_OWNER=$null
    Refuses {Set-GroundMeshConcretePose -SendCommand $mock -Plan $plan} 'Concrete fixture requires shared game ownership.'
    Write-Host 'Actual concrete fixture helper CPU cases PASS; installed admission/captures remain pending.'
} finally {
    $env:LOCK_OWNER=$oldOwner
}
