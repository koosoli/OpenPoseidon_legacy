param(
    [switch]$SampleDriverMemory,
    [switch]$AllocationReport,
    [switch]$DiagnosticOnly,
    [string]$Label = 'dayz-resource-convergence',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if ($DiagnosticOnly) { $AllocationReport = $true }
if (!$env:LOCK_OWNER) {
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $lockArgs = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe, '-NoProfile', '-File',
        $PSCommandPath, '-Label', $Label, '-GameDir', $GameDir)
    if ($SampleDriverMemory) { $lockArgs += '-SampleDriverMemory' }
    if ($AllocationReport) { $lockArgs += '-AllocationReport' }
    if ($DiagnosticOnly) { $lockArgs += '-DiagnosticOnly' }
    $env:LOCK_OWNER = 'DayZ resource convergence'
    try {
        & 'C:\Program Files\Git\bin\bash.exe' @lockArgs
        if ($LASTEXITCODE) { throw "Convergence test exited $LASTEXITCODE" }
    } finally { Remove-Item Env:LOCK_OWNER }
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root ('build/stream-residency/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss))
$profile = Join-Path $out 'user'
New-Item -ItemType Directory -Force $profile | Out-Null
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$world = (Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
$log = Join-Path $out 'engine.log'
$exe = Join-Path $GameDir 'OpenPoseidon.exe'
$dll = Join-Path $GameDir 'wgpu_renderer.dll'
$baseX = 5704.58; $baseZ = 2523.37; $baseY = 73.74; $azimuth = 260.9; $elevation = -3.8
$route = @(5804.58, 5904.58, 6004.58, 6104.58, 6004.58, 5904.58, 5804.58, 5704.58)
$resourceFields = @('geometryLiveBytes', 'geometryCapacityBytes', 'geometryRetiredBytes',
    'objectTextureBytes', 'objectTextureCount', 'backendAllocationBytes')
# HAL aggregate also includes per-frame staging/in-flight/fixed renderer resources.
# Keep its growth comparison and raw samples, but do not mistake transient changes for object warmup.
$persistentFields = @($resourceFields | Where-Object { $_ -ne 'backendAllocationBytes' })
$savedEnvironment = @{}
$environment = @{
    POSEIDON_USER_DIR = $profile
    WGR_OBJECT_STREAM_TEST_BUDGET = '1'; WGR_OBJECT_STREAM_MAX_OBJECTS = '20000'
    WGR_OBJECT_STREAM_GPU_BUDGET = '0'; WGR_GEO_POOL_GROWTH = '2.0'; WGR_GEO_POOL_PRESSURE_GROWTH = '0'
    WGR_OBJECT_STREAM_PBO = '0'; WGR_OBJECT_STREAM_PBO_TEXTURES = '0'
    WGR_NATIVE_DDS_PREPARE = '0'; WGR_NATIVE_DDS_BC3_ONLY = '0'; WGR_SIMULATION_RESIDENCY = '0'
    WGR_ADAPTIVE_TEXTURE_DETAIL = '0'; WGR_LOD_GOVERNOR_RANGE = '1'; WGR_SKY_VOLUME_INCREMENTAL = '1'
    WGR_PASS1_STATS = '1'; WGR_RESIDENCY_TRACE = '1'
}
# Remove inherited experiments, then restore the caller's environment on exit.
# Ordinary terrain paging remains the installed engine default.
$clearNames = @((Get-ChildItem Env: | Where-Object {
    $_.Name -like 'WGR_*' -or $_.Name -like 'POSEIDON_REFORGER_*'
} | ForEach-Object { $_.Name })) + @('POSEIDON_USER_DIR', 'POSEIDON_TEST_RAIN')
$changedNames = @($clearNames + @($environment.Keys) | Select-Object -Unique)
foreach ($name in $changedNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
$p = $null; $client = $null; $gpuSampler = $null
try {
    foreach ($name in $clearNames) { Remove-Item ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    @{
        scriptSha256 = (Get-FileHash $PSCommandPath).Hash; head = (& git -C $root rev-parse HEAD)
        installed = (Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt')); hashes = @(Get-FileHash $exe, $dll)
        corpus = 'DayZ'; pose = @($baseX, $baseZ, $baseY, $azimuth, $elevation); routeX = $route; cycles = 3
        environment = $environment; driverSampled = [bool]$SampleDriverMemory
        quietSamples = 10; quietTimeoutSeconds = 30; routePoseHoldSeconds = 3
        growthAllowance = 'max(1% of return2, 8 MiB); texture count max(1%, 8)'
        processScope = 'PrivateMemorySize64 and WorkingSet64 of owned game PID; diagnostic, not a GPU convergence proof'
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
    if ($SampleDriverMemory) {
        $smi = Get-Command nvidia-smi -ErrorAction Stop
        $gpuSampler = Start-Process -FilePath $smi.Source -WindowStyle Hidden -PassThru -ArgumentList @(
            '--query-gpu=timestamp,index,memory.total,memory.used,utilization.gpu,utilization.memory,clocks.gr,clocks.mem,power.draw,temperature.gpu,pstate',
            '--format=csv,noheader,nounits', '--loop-ms=1000') -RedirectStandardOutput (Join-Path $out 'whole-gpu-memory.csv') -RedirectStandardError (Join-Path $out 'whole-gpu-memory-errors.txt')
        'Whole GPU including other applications, sampled each second; not per-process VRAM.' | Set-Content (Join-Path $out 'whole-gpu-memory-scope.txt')
    }
    function Number($value) { return $value.ToString([Globalization.CultureInfo]::InvariantCulture) }
    function Pose($x) { return ((@($x, $baseZ, $baseY, $azimuth, $elevation) | ForEach-Object { Number $_ }) -join ' ') }
    $p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @(
        '--render=wgpu', '--window', '--dev', '--width', '1280', '--height', '720', '--vd', '2000',
        '--harness', [string]$port, '--capture-metrics', ('"' + (Join-Path $out 'capture.json') + '"'),
        '--test-world-freefly', (Number $baseX), (Number $baseZ), (Number $baseY), (Number $azimuth), (Number $elevation),
        '--test-world', ('"' + $world + '"'), '--addon-root', '"D:\SteamLibrary\steamapps\common\DayZ\Addons"',
        '--test-world-hour', '10', '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
    $null = $p.Handle; $client = [Net.Sockets.TcpClient]::new(); $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (!$client.Connected) {
        try { $client.Connect('127.0.0.1', $port) }
        catch { if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw }; Start-Sleep -Milliseconds 250 }
    }
    $stream = $client.GetStream(); $stream.ReadTimeout = 30000
    $reader = [IO.StreamReader]::new($stream); $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do {
            $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }
            $reply = $line | ConvertFrom-Json
        } while ($null -eq $reply.ok)
        ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl')
        $line | Add-Content (Join-Path $out 'harness.jsonl')
        if (!$reply.ok) { throw $line }; return $reply
    }
    function Sample($phase) {
        if ($p.HasExited) { throw 'Owned game exited before completion' }
        $state = Send @{cmd = 'stream_residency'}
        if (!$state.gpuMemoryValid) { throw 'GPU memory snapshot unavailable' }
        foreach ($field in $resourceFields) { if ($null -eq $state.$field) { throw "Missing resource field $field" } }
        foreach ($field in @('sourceEnvelopeBytes', 'sourceEnvelopeScans', 'sourceEnvelopePublished', 'sourceEnvelopeMs')) {
            if ($null -eq $state.$field -or $state.$field -ne 0) { throw "Default-off source-envelope work changed: $field" }
        }
        $p.Refresh()
        $sample = [pscustomobject]@{utc = [DateTime]::UtcNow.ToString('o'); phase = $phase; state = $state
            processPrivateBytes = $p.PrivateMemorySize64; processWorkingSetBytes = $p.WorkingSet64}
        $sample | ConvertTo-Json -Depth 5 -Compress | Add-Content (Join-Path $out 'samples.jsonl')
        return $sample
    }
    function Assert-BaseCoverage($state) {
        if (!$state.valid -or $state.pending -or $state.resident -ne 19923 -or $state.desired -ne 19923 -or
            $state.budget -ne 20000 -or $state.centerX -ne 92 -or $state.centerZ -ne 40) {
            throw 'Original-pose 19,923-object coverage was not preserved'
        }
    }
    function Wait-Settled($phase) {
        $deadline = [DateTime]::UtcNow.AddSeconds(240)
        do {
            Start-Sleep -Seconds 2
            $sample = Sample $phase
            if ([DateTime]::UtcNow -gt $deadline) { throw 'Logical residency failed to settle in 240 seconds' }
        } while (!$sample.state.valid -or $sample.state.pending -or $sample.state.resident -eq 0)
        Assert-BaseCoverage $sample.state
        return $sample
    }
    function Wait-Quiet($phase) {
        $deadline = [DateTime]::UtcNow.AddSeconds(30); $previous = $null; $streak = 0
        do {
            Start-Sleep -Seconds 1
            $sample = Sample $phase; Assert-BaseCoverage $sample.state
            $same = $null -ne $previous
            if ($same) { foreach ($field in $persistentFields) { if ($sample.state.$field -ne $previous.state.$field) { $same = $false; break } } }
            $streak = if ($same) { $streak + 1 } else { 1 }
            $previous = $sample
            if ($streak -ge 10) { return [pscustomobject]@{quiet = $true; status = 'Plateau'; last = $sample; consecutiveSamples = $streak} }
        } while ([DateTime]::UtcNow -lt $deadline)
        return [pscustomobject]@{quiet = $false; status = 'PendingWarmup'; last = $sample; consecutiveSamples = $streak}
    }
    $initialCold = Wait-Settled 'initial-cold'; $initialQuiet = Wait-Quiet 'initial-quiet'
    $returns = @()
    $cycleCount = if ($DiagnosticOnly) { 0 } else { 3 }
    for ($cycle = 1; $cycle -le $cycleCount; $cycle++) {
        $null = Send @{cmd = 'exec'; code = ('logInfo "CONVERGENCE_ROUTE_' + $cycle + '_BEGIN"; triPerfReset 0')}
        foreach ($x in $route) {
            $reply = Send @{cmd = 'eval'; code = ('triFreeFlyPose "' + (Pose $x) + '"')}
            if ($reply.result -notmatch 'OK') { throw 'Traversal pose refused' }
            Start-Sleep -Seconds 3
            $null = Sample ('route-' + $cycle)
        }
        $timing = Send @{cmd = 'eval'; code = 'triPerfStats 0'}
        $null = Send @{cmd = 'exec'; code = ('logInfo "CONVERGENCE_ROUTE_' + $cycle + '_END"')}
        $settled = Wait-Settled ('return-' + $cycle + '-logical')
        $quiet = Wait-Quiet ('return-' + $cycle + '-quiet')
        $returns += [pscustomobject]@{cycle = $cycle; logical = $settled; quiet = $quiet; routeTiming = $timing}
    }
    $comparison = @(); $bounded = $true
    foreach ($field in $(if ($DiagnosticOnly) { @() } else { $resourceFields })) {
        $second = [double]$returns[1].quiet.last.state.$field; $third = [double]$returns[2].quiet.last.state.$field
        $fixed = if ($field -eq 'objectTextureCount') { 8 } else { 8 * 1024 * 1024 }
        $allowance = [Math]::Max($fixed, [Math]::Ceiling($second * 0.01)); $growth = $third - $second
        $accepted = $growth -le $allowance
        $comparison += [pscustomobject]@{field = $field; return2 = $second; return3 = $third; growth = $growth; allowedGrowth = $allowance; accepted = $accepted}
        if (!$accepted) { $bounded = $false }
    }
    $lateQuiet = !$DiagnosticOnly -and $returns[1].quiet.quiet -and $returns[2].quiet.quiet
    $retiredClear = !$DiagnosticOnly -and $returns[1].quiet.last.state.geometryRetiredBytes -eq 0 -and $returns[2].quiet.last.state.geometryRetiredBytes -eq 0
    $converged = $bounded -and $lateQuiet -and $retiredClear
    $status = if (!$lateQuiet) { 'PendingWarmup' } elseif (!$converged) { 'ResourceGrowth' } else { 'Converged' }
    if ($DiagnosticOnly) { $status = 'AllocationDiagnosticOnly' }
    $allocation = $null
    if ($AllocationReport) {
        function Read-Allocation($request) {
            $started = Send $request
            if ($started.status -ne 'Pending' -or $started.requestId -lt 1) { throw 'Allocation request was not accepted asynchronously' }
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                Start-Sleep -Milliseconds 250
                $report = Send @{cmd='geometry_allocations'; requestId=$started.requestId}
                if ([DateTime]::UtcNow -gt $deadline) { throw 'Allocation report did not complete' }
            } while ($report.status -eq 'Pending')
            if ($report.status -ne 'Ready' -or !$report.valid -or $report.invalidRanges -ne 0 -or $report.unresolvedNames -ne 0) { throw 'Allocation report failed validity checks' }
            if (($report.uniqueVertexBytes + $report.uniqueIndexBytes) -gt $report.poolLiveBytes) { throw 'Unique retained mesh allocation exceeds the whole pool' }
            foreach ($field in @('overlapWithinInspectedMeshes', 'overlapWithinInspectedVertexBytes', 'overlapWithinInspectedIndexBytes', 'ownershipCoverageComplete')) {
                if (!$report.PSObject.Properties[$field]) { throw "Allocation report omitted ownership attribution field $field" }
            }
            if ($report.overlapWithinInspectedMeshes -gt $report.uniqueMeshes -or
                $report.overlapWithinInspectedVertexBytes -gt $report.uniqueVertexBytes -or
                $report.overlapWithinInspectedIndexBytes -gt $report.uniqueIndexBytes) { throw 'Inspected allocation overlap exceeds its unique union' }
            if ($report.ownershipCoverageComplete) { throw 'Diagnostic subset incorrectly claimed complete allocation ownership' }
            if ($report.registryEntriesVisited -gt $report.maxModels -or
                ($report.modelsRequested + $report.ineligibleEntriesSkipped) -ne $report.registryEntriesVisited) { throw 'Registry sampling and negative-cache accounting disagree' }
            if ($report.missingModels -ne 0 -or $report.missingMeshes -ne 0) { throw 'Settled eligible retained models or meshes are missing' }
            return $report
        }
        # Diagnostics run after timed routes and quiet samples; they do not prove usage or reclaimability.
        $allocation = Read-Allocation @{cmd='geometry_allocations'}
        if (@($allocation.rows).Count -lt 2 -or $allocation.uniqueMeshes -lt 1) { throw 'No meaningful retained geometry allocation rows' }
        $boundedReport = Read-Allocation @{cmd='geometry_allocations'; maxModels=1; maxRows=1; maxSectionVisits=65536}
        if (!$boundedReport.truncated -or @($boundedReport.rows).Count -gt 1) { throw 'Bounded report truncation was not preserved' }
        $stale = Send @{cmd='geometry_allocations'; requestId=$allocation.requestId}
        if ($stale.status -ne 'Invalid') { throw 'Superseded report ID remained claimable' }
        $allocation | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-allocations.json')
        $boundedReport | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-allocations-bounded.json')
    }
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'returned-settled.png')}
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) { throw 'Game failed to exit cleanly' }
    if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Renderer error' }
    # Publish verdict only after the owned process exits cleanly. Process/driver
    # measurements remain raw evidence; resident count alone proves no memory bound.
    @{passed = ($converged -or $DiagnosticOnly); convergenceMeasured = !$DiagnosticOnly; status = $status; initialCold = $initialCold; initialQuiet = $initialQuiet
        returns = $returns; comparison = $comparison; lateQuiet = $lateQuiet; retiredClear = $retiredClear
        processScope = 'Diagnostic private/RSS samples retained; GPU convergence does not establish process-memory convergence'
        allocationReport = $allocation
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
    Write-Output "Resource convergence evidence: $out ($status)"
    if (!$converged -and !$DiagnosticOnly) { throw "Convergence not established: $status (see result.json and raw samples)" }
} finally {
    if ($gpuSampler -and !$gpuSampler.HasExited) { Stop-Process -Id $gpuSampler.Id }
    if ($client) { $client.Dispose() }
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
    foreach ($name in $changedNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
