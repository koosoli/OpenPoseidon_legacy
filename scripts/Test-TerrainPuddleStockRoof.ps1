# Pure mocked-TCP test. Does not launch or connect to a game.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
$savedLockOwner = $env:LOCK_OWNER
try {
    $env:LOCK_OWNER = 'CPU stock roof helper test (mock RPC only)'
    $mock = {
        param($request)
        switch ($request.cmd) {
            'water_bathymetry' { return @{ samples = @(@($request.x,$request.z,204.72),@(0,0,999)) } }
            'exec' { return @{ ok = $true } }
            'eval' {
                switch ($request.code) {
                    'typeOf puddleStockRoof' { return @{ result = 'CampEastC' } }
                    'getPosASL puddleStockRoof' { return @{ result = '[9481.25,3006.25,207.550052871704]' } }
                    'puddleStockRoof' { return @{ result = 'NOID stan_eastc' } }
                    'isNull puddleStockRoof' { return @{ result = 'true' } }
                }
            }
        }
        throw 'Unexpected mock request.'
    }
    $fixture = New-TerrainPuddleStockRoof -SendCommand $mock
    if ($fixture.coveredCamera -notmatch '^9481.25 3004.75 205.72' -or $fixture.exposedCamera -notmatch '^9493.25 3004.75 205.72') {
        throw 'Camera ASL heights were not derived from runtime ground.'
    }
    Remove-TerrainPuddleStockRoof -SendCommand $mock
    $wrongClass = { param($request); if ($request.code -eq 'typeOf puddleStockRoof') { return @{ result = 'CampEmpty' } }; & $mock $request }
    $rejected = $false
    try { $null = New-TerrainPuddleStockRoof -SendCommand $wrongClass } catch { $rejected = $true }
    if (!$rejected) { throw 'Floor-bearing wrong stock class accepted.' }
    $wrongModel = { param($request); if ($request.code -eq 'puddleStockRoof') { return @{ result = 'NOID stan_inside.p3d' } }; & $mock $request }
    $rejected = $false
    try { $null = New-TerrainPuddleStockRoof -SendCommand $wrongModel } catch { $rejected = $true }
    if (!$rejected) { throw 'Wrong loaded model accepted.' }
    $wrongPosition = { param($request); if ($request.code -eq 'getPosASL puddleStockRoof') { return @{ result = '[9500,3018,207.55]' } }; & $mock $request }
    $rejected = $false
    try { $null = New-TerrainPuddleStockRoof -SendCommand $wrongPosition } catch { $rejected = $true }
    if (!$rejected) { throw 'Displaced roof accepted.' }
    Write-Host 'Stock roof helper: runtime height cameras, exact class/model/position, deletion and refusals passed with mock RPC.'
} finally { $env:LOCK_OWNER = $savedLockOwner }
