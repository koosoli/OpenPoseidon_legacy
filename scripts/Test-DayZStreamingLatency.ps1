param(
    [ValidateSet('Disabled', 'Cold')][string]$Mode = 'Disabled',
    [string]$Label = 'dayz-streaming-latency',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) {
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $env:LOCK_OWNER = 'DayZ bounded streaming latency screening'
    try {
        & 'C:\Program Files\Git\bin\bash.exe' (Join-Path $PSScriptRoot 'with-game-lock.sh') $shellExe '-NoProfile' '-File' $PSCommandPath '-Mode' $Mode '-Label' $Label '-GameDir' $GameDir
        if ($LASTEXITCODE) { throw "Latency screening exited $LASTEXITCODE" }
    } finally { Remove-Item Env:LOCK_OWNER }
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root ('build/stream-residency/' + $Label + '-' + $Mode.ToLowerInvariant() + '-' + (Get-Date -Format yyyyMMdd-HHmmss))
$profile = Join-Path $out 'user'; $mission = Join-Path $out 'latency.eden'
New-Item -ItemType Directory -Force -Path $profile, $mission | Out-Null
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
@'
version=11;
class Mission {randomSeed=1234;
 class Intel {year=1985;month=6;day=21;hour=10;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;
  class Item0 {position[]={5704.58,0,2523.37};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";leader=1;skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";};
 };};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ | Set-Content (Join-Path $mission 'mission.sqm')
$world = (Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
$log = Join-Path $out 'engine.log'; $exe = Join-Path $GameDir 'OpenPoseidon.exe'; $dll = Join-Path $GameDir 'wgpu_renderer.dll'
$baseX = 5704.58; $baseZ = 2523.37; $baseY = 73.74; $azimuth = 260.9; $elevation = -3.8
# Exact eight convergence traversal poses, followed by three stationary return holds.
$route = @(5804.58, 5904.58, 6004.58, 6104.58, 6004.58, 5904.58, 5804.58, 5704.58, 5704.58, 5704.58, 5704.58)
$cold = if ($Mode -eq 'Cold') { '1' } else { '0' }
$environment = @{
    POSEIDON_USER_DIR = $profile; POSEIDON_MODEL_DDC = '0'
    WGR_SIMULATION_RESIDENCY = $cold; WGR_SIMULATION_POSITIVE_COLD = $cold
    WGR_OBJECT_STREAM_TEST_BUDGET = '1'; WGR_OBJECT_STREAM_MAX_OBJECTS = '20000'
    WGR_OBJECT_STREAM_GPU_BUDGET = '0'; WGR_GEO_POOL_GROWTH = '2.0'; WGR_GEO_POOL_PRESSURE_GROWTH = '0'
    WGR_OBJECT_STREAM_PBO = '1'; WGR_OBJECT_STREAM_PBO_TEXTURES = '0'
    WGR_NATIVE_DDS_PREPARE = '0'; WGR_NATIVE_DDS_BC3_ONLY = '0'
    WGR_ADAPTIVE_TEXTURE_DETAIL = '0'; WGR_LOD_GOVERNOR_RANGE = '1'; WGR_SKY_VOLUME_INCREMENTAL = '1'
    WGR_TEXTURE_STREAM_STATS = '0'
}
# Shape trace enables on variable presence, so leave it absent in both modes.
$clearNames = @((Get-ChildItem Env: | Where-Object { $_.Name -like 'WGR_*' -or $_.Name -like 'POSEIDON_*' } | ForEach-Object { $_.Name }))
$changedNames = @($clearNames + @($environment.Keys) | Select-Object -Unique)
$savedEnvironment = @{}; foreach ($name in $changedNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
$p = $null; $client = $null
try {
    foreach ($name in $clearNames) { Remove-Item ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $initialHashes = @(Get-FileHash $exe, $dll)
    @{
        mode = $Mode; head = (& git -C $root rev-parse HEAD); scriptSha256 = (Get-FileHash $PSCommandPath).Hash
        installed = (Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt')); hashes = $initialHashes
        world = $world; environment = $environment; clearedEnvironmentNames = $clearNames
        pose = @($baseX, $baseZ, $baseY, $azimuth, $elevation); routeX = $route; poseHoldSeconds = 3
        width = 1280; height = 720; msaaSamples = 4; fpsCap = 60; viewDistance = 2000
        routeScope = 'Eight convergence400m out/return poses plus three stationary recovery holds; one process per invocation'
        measurementScope = 'Bounded engine16384-frame route capture; screening only, no FPS acceptance, vanilla parity or full cold-cache proof'
        processScope = 'Owned PID private/RSS samples; sparse owner-pump values are observations, not percentiles'
    } | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $out 'provenance.json')
    function Number($value) { return $value.ToString([Globalization.CultureInfo]::InvariantCulture) }
    function Pose($x) { return ((@($x, $baseZ, $baseY, $azimuth, $elevation) | ForEach-Object { Number $_ }) -join ' ') }
    $p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @(
        '--render=wgpu', '--window', '--dev', '--width', '1280', '--height', '720', '--vd', '2000',
        '--harness', [string]$port, '--capture-metrics', ('"' + (Join-Path $out 'capture.json') + '"'),
        '--test-world-freefly', (Number $baseX), (Number $baseZ), (Number $baseY), (Number $azimuth), (Number $elevation),
        '--test-world', ('"' + $world + '"'), '--addon-root', '"D:\SteamLibrary\steamapps\common\DayZ\Addons"',
        '--test-world-hour', '10', '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
    $null = $p.Handle; $client = [Net.Sockets.TcpClient]::new(); $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while (!$client.Connected) {
        try { $client.Connect('127.0.0.1', $port) }
        catch { if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw }; Start-Sleep -Milliseconds 250 }
    }
    $stream = $client.GetStream(); $stream.ReadTimeout = 30000
    $reader = [IO.StreamReader]::new($stream); $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do { $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $reply = $line | ConvertFrom-Json } while ($null -eq $reply.ok)
        ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl')
        $line | Add-Content (Join-Path $out 'harness.jsonl')
        if (!$reply.ok) { throw $line }; return $reply
    }
    function Sample($phase, $poseIndex = -1) {
        if ($p.HasExited) { throw 'Owned game exited before completion' }
        $camera = Send @{cmd = 'stream_residency'}; $simulation = Send @{cmd = 'stream_simulation_residency'}
        if (!$camera.valid -or $camera.budget -ne 20000 -or $camera.required -ne 0 -or $simulation.requiredObjects -ne 0) { throw 'Coverage validity, budget or required pins changed' }
        if ($Mode -eq 'Disabled' -and ($simulation.status -ne 'Disabled' -or $simulation.positiveColdRequested -ne 0 -or $simulation.positiveColdCreated -ne 0)) { throw 'Disabled mode performed simulation residency or cold admission' }
        if ($Mode -eq 'Cold' -and (($simulation.metadataComplete -and $simulation.status -ne 'CapacityExceeded') -or $simulation.readyRegions -ne 0 -or $simulation.readyQueries -ne 0)) { throw 'Partial production service fabricated full readiness' }
        $p.Refresh()
        $sample = [pscustomobject]@{utc = [DateTime]::UtcNow.ToString('o'); phase = $phase; poseIndex = $poseIndex
            camera = $camera; simulation = $simulation; processPrivateBytes = $p.PrivateMemorySize64; processWorkingSetBytes = $p.WorkingSet64}
        $sample | ConvertTo-Json -Depth 6 -Compress | Add-Content (Join-Path $out 'samples.jsonl'); return $sample
    }
    function Wait-Base($phase) {
        $deadline = [DateTime]::UtcNow.AddSeconds(240)
        do {
            Start-Sleep -Seconds 2; $sample = Sample $phase
            if ([DateTime]::UtcNow -gt $deadline) { throw 'Original pose failed to settle in240seconds' }
        } while ($sample.camera.pending -or ($Mode -eq 'Cold' -and !$sample.simulation.metadataComplete))
        if ($sample.camera.resident -ne 19923 -or $sample.camera.desired -ne 19923 -or $sample.camera.centerX -ne 92 -or $sample.camera.centerZ -ne 40) { throw 'Original19,923-camera-object coverage changed' }
        return $sample
    }
    $baseline = Wait-Base 'baseline'
    $armed = Send @{cmd = 'eval'; code = 'triPerfCapture 16384'}
    if ($armed.result.Trim('"') -ne 'OK') { throw 'Engine frame capture did not arm' }
    $routeSamples = @()
    for ($i = 0; $i -lt $route.Count; $i++) {
        $pose = Send @{cmd = 'eval'; code = ('triFreeFlyPose "' + (Pose $route[$i]) + '"')}
        if ($pose.result.Trim('"') -ne 'OK') { throw 'Traversal pose refused' }
        Start-Sleep -Seconds 3; $routeSamples += Sample 'route' $i
    }
    $stopped = Send @{cmd = 'eval'; code = 'triPerfCapture 0'}
    if ($stopped.result.Trim('"') -ne 'OK') { throw 'Engine capture incomplete or dropped frames' }
    $returned = Wait-Base 'returned'
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'returned-settled.png')}
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) { throw 'Owned game failed to exit cleanly' }
    if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Renderer error' }
    $finalHashes = @(Get-FileHash $exe, $dll)
    for ($i = 0; $i -lt $initialHashes.Count; $i++) { if ($initialHashes[$i].Hash -ne $finalHashes[$i].Hash) { throw 'Installed binary changed during screening' } }
    $begins = @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture begin n=(\d+) dropped=(\d+)')
    $ends = @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture end')
    if ($begins.Count -ne 1 -or $ends.Count -ne 1) { throw 'Expected exactly one complete engine frame capture' }
    $count = [int]$begins[0].Matches[0].Groups[1].Value; $dropped = [int]$begins[0].Matches[0].Groups[2].Value
    if ($count -lt 1 -or $count -gt 16384 -or $dropped -ne 0) { throw 'Engine capture count/drop evidence invalid' }
    $culture = [Globalization.CultureInfo]::InvariantCulture; $raw = @(); $phases = @()
    foreach ($row in @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture offset=(\d+) ms=(.*)$')) {
        if ([int]$row.Matches[0].Groups[1].Value -ne $raw.Count) { throw 'Engine raw frame series has a gap' }
        foreach ($value in $row.Matches[0].Groups[2].Value.Split(',')) { $raw += [double]::Parse($value, $culture) }
    }
    if ($raw.Count -ne $count) { throw 'Engine raw frame series does not match capture count' }
    foreach ($row in @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture phase=(\S+) avg_ms=([\d.]+) p95_ms=([\d.]+) max_ms=([\d.]+)')) {
        $m = $row.Matches[0]; $phases += [pscustomobject]@{phase = $m.Groups[1].Value; avgMs = [double]::Parse($m.Groups[2].Value, $culture); p95Ms = [double]::Parse($m.Groups[3].Value, $culture); maxMs = [double]::Parse($m.Groups[4].Value, $culture)}
    }
    $expectedPhases = @('setup', 'sim:step', 'drw:init', 'drw:prep', 'land:gnd', 'land:obj', 'drw:land', 'drw:obj', 'drw:post', 'hud', 'sound', 'swap')
    if ($phases.Count -ne $expectedPhases.Count) { throw 'Engine phase timing evidence incomplete' }
    foreach ($phase in $expectedPhases) { if (@($phases | Where-Object { $_.phase -eq $phase }).Count -ne 1) { throw "Engine phase timing missing or duplicated: $phase" } }
    $raw | ConvertTo-Json | Set-Content (Join-Path $out 'route-frame-ms.json')
    @{passed = $true; mode = $Mode; screeningOnly = $true; performanceAccepted = $false
        frameCount = $count; dropped = $dropped; enginePhaseTiming = $phases; baseline = $baseline; returned = $returned
        routeSamples = $routeSamples; postExitHashes = $finalHashes
        limitation = 'No automatic performance threshold, vanilla parity, cold-cache guarantee or whole-world query readiness; compare matched separate mode processes'
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
    Write-Output "DayZ latency screening evidence: $out"
} finally {
    if ($client) { $client.Dispose() }
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
    foreach ($name in $changedNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
