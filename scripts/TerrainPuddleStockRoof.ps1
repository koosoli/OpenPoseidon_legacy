# Dot-source from an already owned, isolated installed-game TCP harness.
# This helper never launches, deploys, owns a process or writes a game/profile file.
function Get-TerrainPuddleFixtureHeight {
    param([scriptblock]$SendCommand,[double]$X,[double]$Z)
    $heightMap = & $SendCommand @{ cmd = 'water_bathymetry'; x = $X; z = $Z }
    $centre = @($heightMap.samples | Where-Object {
        $_.Count -eq 3 -and [Math]::Abs([double]$_[0]-$X) -lt 0.01 -and [Math]::Abs([double]$_[1]-$Z) -lt 0.01 })
    if ($centre.Count -ne 1) { throw 'Runtime terrain-height query returned no unique centre sample.' }
    $height = [double]$centre[0][2]
    if ([double]::IsNaN($height) -or [double]::IsInfinity($height)) { throw 'Runtime terrain height is nonfinite.' }
    return $height
}

function New-TerrainPuddleStockRoof {
    [CmdletBinding()]
    param([Parameter(Mandatory)][scriptblock]$SendCommand)
    if (!$env:LOCK_OWNER) { throw 'Stock roof fixture requires the shared game lock.' }
    # Verified installed stock class/model: CampEastC -> stan_eastC.p3d.
    # Centre is the previously validated native Dirt_02 patch, not a new material
    # guess. Explicit placement avoids createVehicle's free-position search.
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $ground = Get-TerrainPuddleFixtureHeight $SendCommand 9481.25 3006.25
    # Engine LoadOptimized/SaveOptimized roundtrip preserves this model lower
    # bound and every audited roof height; recentering was checked offline.
    $modelMinimumY = -2.8100528717041
    $originHeight = $ground - $modelMinimumY + 0.02
    $spawnCode = 'puddleStockRoof = "CampEastC" createVehicle [9481.25,3006.25,0]; puddleStockRoof setDir 0; puddleStockRoof setPosASL [9481.25,3006.25,' + $originHeight.ToString('R',$culture) + ']; puddleStockRoof allowDamage false'
    $null = & $SendCommand @{ cmd = 'exec'; code = $spawnCode }
    $class = & $SendCommand @{ cmd = 'eval'; code = 'typeOf puddleStockRoof' }
    if (([string]$class.result).Trim('"') -cne 'CampEastC') { throw 'The verified stock roof class did not spawn.' }
    $position = & $SendCommand @{ cmd = 'eval'; code = 'getPosASL puddleStockRoof' }
    $actual = ([string]$position.result).Trim('"') | ConvertFrom-Json
    if ($actual.Count -ne 3) { throw 'Stock roof returned no actual ASL position.' }
    $expected = @(9481.25,3006.25,$originHeight)
    for ($i=0; $i -lt 3; ++$i) {
        $number = [double]$actual[$i]
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or [Math]::Abs($number-$expected[$i]) -gt 0.1) {
            throw 'Stock roof actual placement differs from the requested fixture.'
        }
    }
    $object = & $SendCommand @{ cmd = 'eval'; code = 'puddleStockRoof' }
    # Object::GetDebugName strips the extension via GetFilename.
    if (([string]$object.result) -notmatch '(^|[\\/\s:])stan_eastc(?:\.p3d)?(?:$|")') {
        throw 'Created object debug name does not identify the verified stock model.'
    }
    $coveredGround = Get-TerrainPuddleFixtureHeight $SendCommand 9481.25 3004.75
    $exposedGround = Get-TerrainPuddleFixtureHeight $SendCommand 9493.25 3004.75
    return @{ class = 'CampEastC'; model = 'stan_eastC.p3d'; actualPosition = $actual; objectDebugName = $object.result;
        modelMinimumY = $modelMinimumY; centreGroundHeight = $ground; coveredGroundHeight = $coveredGround; exposedGroundHeight = $exposedGround;
        coveredCamera = ('9481.25 3004.75 '+($coveredGround+1.0).ToString('R',$culture)+' 0 -55');
        exposedCamera = ('9493.25 3004.75 '+($exposedGround+1.0).ToString('R',$culture)+' 0 -55');
        status = 'spawned-not-runtime-accepted';
        scope = 'Known central floor-free first-LOD region; actual retained roof depth and visible terrain response still need covered/removed controls.' }
}

function Remove-TerrainPuddleStockRoof {
    [CmdletBinding()]
    param([Parameter(Mandatory)][scriptblock]$SendCommand)
    if (!$env:LOCK_OWNER) { throw 'Stock roof fixture requires the shared game lock.' }
    $null = & $SendCommand @{ cmd = 'exec'; code = 'deleteVehicle puddleStockRoof' }
    $removed = & $SendCommand @{ cmd = 'eval'; code = 'isNull puddleStockRoof' }
    if (([string]$removed.result).Trim('"') -ne 'true') { throw 'Fixture roof did not leave the isolated mission.' }
}
