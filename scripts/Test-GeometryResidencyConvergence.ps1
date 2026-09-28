param(
    [switch]$SampleDriverMemory,
    [switch]$AllocationReport,
    [switch]$DiagnosticOnly,
    [switch]$LodDemandReport,
    [switch]$OwnerLedger,
    [switch]$MeshAck,
    [switch]$RegisteredRefs,
    [switch]$ParkedGeometryReport,
    [switch]$PressureCacheTrim,
    [string]$Label = 'dayz-resource-convergence',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if ($MeshAck -or $RegisteredRefs -or $ParkedGeometryReport) { $OwnerLedger = $true }
if ($DiagnosticOnly -or $LodDemandReport -or $OwnerLedger) { $AllocationReport = $true }
if (!$env:LOCK_OWNER) {
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $lockArgs = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe, '-NoProfile', '-File',
        $PSCommandPath, '-Label', $Label, '-GameDir', $GameDir)
    if ($SampleDriverMemory) { $lockArgs += '-SampleDriverMemory' }
    if ($AllocationReport) { $lockArgs += '-AllocationReport' }
    if ($DiagnosticOnly) { $lockArgs += '-DiagnosticOnly' }
    if ($LodDemandReport) { $lockArgs += '-LodDemandReport' }
    if ($OwnerLedger) { $lockArgs += '-OwnerLedger' }
    if ($MeshAck) { $lockArgs += '-MeshAck' }
    if ($RegisteredRefs) { $lockArgs += '-RegisteredRefs' }
    if ($ParkedGeometryReport) { $lockArgs += '-ParkedGeometryReport' }
    if ($PressureCacheTrim) { $lockArgs += '-PressureCacheTrim' }
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
    WGR_OBJECT_STREAM_PRESSURE_CACHE_TRIM = $(if ($PressureCacheTrim) { '1' } else { '0' })
}
if ($LodDemandReport) { $environment.WGR_GEOMETRY_LOD_FEEDBACK = '1' }
if ($OwnerLedger) { $environment.WGR_GEOMETRY_OWNER_LEDGER = '1' }
if ($MeshAck) { $environment.WGR_GEOMETRY_MESH_ACK = '1' }
if ($RegisteredRefs) { $environment.WGR_GEOMETRY_REGISTERED_REFS = '1' }
$environment.WGR_GEOMETRY_PARKED_REPORT = $(if ($ParkedGeometryReport) { '1' } else { '0' })
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
        meshAck = [bool]$MeshAck
        registeredRefs = [bool]$RegisteredRefs
        parkedGeometryReport = [bool]$ParkedGeometryReport
        installed = (Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt')); hashes = @(Get-FileHash $exe, $dll)
        corpus = 'DayZ'; pose = @($baseX, $baseZ, $baseY, $azimuth, $elevation); routeX = $route; cycles = $(if ($DiagnosticOnly) { 0 } else { 3 })
        environment = $environment; driverSampled = [bool]$SampleDriverMemory
        quietSamples = 10; quietTimeoutSeconds = 30; routePoseHoldSeconds = 3
        perReturnAllocationCuts = [bool]($AllocationReport -and !$DiagnosticOnly)
        perReturnAllocationScope = 'Returns2/3 after quiet samples, outside route timings; bounded extra inter-route interval; sampled asset/LOD rows may be truncated and overlap'
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
        $script:parkedReportObservations = @()
        function Read-Allocation($request, [bool]$finalEvidence = $false) {
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
            $parkedFields = @('parkedReportEnabled','parkedCaptureComplete','parkedJoinComplete','parkedTruncated',
                'parkedModelsVisited','parkedModelsCaptured','parkedMetadataVisits','parkedInvalidEntries','parkedLinkVisits',
                'parkedUniqueAllocations','parkedCpuBorrowedAllocations','parkedCpuBorrowedBytes',
                'parkedUnborrowedAllocations','parkedUnborrowedBytes','parkedOutsideLinkedAllocations','parkedOutsideLinkedBytes',
                'parkedAbsentRetiredAllocations','parkedUnknownAllocations','parkedUnknownKnownBytes','parkedKnownCandidateBytes',
                'parkedRequestKnownCapacityBytes','parkedLinkKnownCapacityBytes')
            foreach ($field in $parkedFields) {
                if ($null -eq $report.$field) { throw "Missing parked candidate field: $field" }
            }
            if ([bool]$report.parkedReportEnabled -ne [bool]$ParkedGeometryReport) { throw 'Parked report activation mismatch' }
            if (!$ParkedGeometryReport) {
                foreach ($field in $parkedFields) { if ($report.$field -ne 0) { throw "OFF parked report unexpectedly populated $field" } }
            } else {
                if ($report.parkedModelsVisited -gt 256 -or $report.parkedModelsCaptured -gt $report.parkedModelsVisited -or
                    $report.parkedMetadataVisits -gt 2048 -or $report.parkedLinkVisits -gt 2048 -or
                    $report.parkedUniqueAllocations -gt 2048) { throw 'Parked report exceeded bounded selection' }
                $classes = [decimal]$report.parkedCpuBorrowedAllocations + [decimal]$report.parkedUnborrowedAllocations +
                    [decimal]$report.parkedOutsideLinkedAllocations + [decimal]$report.parkedAbsentRetiredAllocations + [decimal]$report.parkedUnknownAllocations
                $bytes = [decimal]$report.parkedCpuBorrowedBytes + [decimal]$report.parkedUnborrowedBytes +
                    [decimal]$report.parkedOutsideLinkedBytes + [decimal]$report.parkedUnknownKnownBytes
                if ($classes -ne $report.parkedUniqueAllocations -or $bytes -ne $report.parkedKnownCandidateBytes) {
                    throw 'Parked candidate classes are not a disjoint union'
                }
                if ($report.parkedJoinComplete -and (!$report.parkedCaptureComplete -or $report.parkedTruncated -or $report.parkedUnknownAllocations -ne 0)) {
                    throw 'Parked join incorrectly claimed complete selected attribution'
                }
            }

            if ($ParkedGeometryReport) {
                $observation = @{requestId=$report.requestId}
                foreach ($field in $parkedFields) { $observation[$field]=$report.$field }
                $script:parkedReportObservations += [pscustomobject]$observation
            }
            foreach ($field in @('overlapWithinInspectedMeshes', 'overlapWithinInspectedVertexBytes', 'overlapWithinInspectedIndexBytes', 'ownershipCoverageComplete')) {
                if (!$report.PSObject.Properties[$field]) { throw "Allocation report omitted ownership attribution field $field" }
            }
            if ($report.overlapWithinInspectedMeshes -gt $report.uniqueMeshes -or
                $report.overlapWithinInspectedVertexBytes -gt $report.uniqueVertexBytes -or
                $report.overlapWithinInspectedIndexBytes -gt $report.uniqueIndexBytes) { throw 'Inspected allocation overlap exceeds its unique union' }
            if ($report.ownershipCoverageComplete) { throw 'Diagnostic subset incorrectly claimed complete allocation ownership' }
              if ($OwnerLedger) {
                  foreach ($field in @('persistentPrunedAllocations','persistentPrunedBorrowers',
                      'persistentRetirementCandidates','persistentCleanupKnownCapacityBytes',
                      'persistentDetachedHistory','persistentBorrowerKnownRecordBytes','persistentSelectedDiagnosticPins')) {
                      if ($null -eq $report.$field) { throw "Missing bounded history field: $field" }
                  }
                  if ($report.persistentRetirementCandidates -gt 8192 -or
                      $report.persistentCleanupKnownCapacityBytes -ne 65536 -or
                      $report.persistentSelectedDiagnosticPins -ne 0 -or
                      $report.persistentDetachedHistory -gt $report.persistentTrackedBorrowerRecords) {
                      throw 'Owner-history metadata exceeded its declared bounds or retained a private fixture pin'
                  }
                  if ($finalEvidence -and !$DiagnosticOnly -and ($report.persistentPrunedAllocations -lt 1 -or $report.persistentPrunedBorrowers -lt 1)) {
                      throw 'Traversal did not establish actual allocation and borrower history pruning'
                  }
                if (!$report.persistentAttributionEnabled -or $report.persistentAttributionEpoch -lt 1 -or
                    $report.persistentTrackedAllocations -lt 1 -or $report.persistentCpuBorrowers -lt 1 -or
                    ($report.persistentCpuReferencedVertexBytes + $report.persistentCpuReferencedIndexBytes) -lt 1) {
                    throw 'Persistent owner ledger did not observe actual producer allocations and CPU borrowers'
                }
                if ($report.persistentReclaimableBytes -ne 0 -or
                    ($report.persistentAttributionComplete -and $report.persistentIncompleteReasons -ne 0)) {
                    throw 'Persistent attribution fabricated reclaimability or hid incompleteness'
                }
                if (!$report.rendererMeshFactsEnabled -or !$report.rendererMeshFactsValid -or !$report.rendererMeshFactsComplete -or
                    $report.rendererMeshFactCalls -ne 1 -or $report.rendererMeshFactEpoch -ne $report.persistentAttributionEpoch -or
                    $report.rendererMeshFactsRequested -lt 1 -or $report.rendererMeshFactsInspected -ne $report.rendererMeshFactsRequested -or
                    $report.rendererMeshFactsRequested -gt 8192 -or $report.rendererMeshFactsPresent -lt 1 -or
                    ($report.rendererMeshFactVertexBytes + $report.rendererMeshFactIndexBytes) -lt 1 -or
                    $report.rendererMeshFactsInvalid -ne 0 -or $report.rendererMeshFactsDuplicateHandles -ne 0 -or
                    $report.rendererMeshFactsUnknownMappings -ne 0 -or $report.rendererMeshFactsTruncated -or
                    ($report.rendererMeshFactsPresent + $report.rendererMeshFactsAbsent) -ne $report.rendererMeshFactsInspected) {
                    throw 'Renderer mesh fact probe lacked a complete bounded actual handle cut'
                }
                if (($report.rendererMeshFactsPresentScheduledRetired + $report.rendererMeshFactsAbsentScheduledRetired) -ne $report.rendererMeshFactsScheduledRetired -or
                    $report.rendererMeshFactsAbsentWithPersistentOwners -gt $report.rendererMeshFactsAbsent) {
                    throw 'Renderer presence and persistent owner joins disagree'
                }
                $recordBytes=$report.rendererMeshFactVertexBytes + $report.rendererMeshFactIndexBytes
                if (!$report.rendererMeshRecordScopeValid -or !$report.rendererMeshPoolResidualValid -or
                    $report.rendererMeshRecordPoolGeneration -lt 1 -or
                    $report.rendererMeshLiveRecords -lt $report.rendererMeshFactsPresent -or
                    $recordBytes -gt $report.rendererMeshRecordPoolLiveBytes -or
                    $report.rendererMeshPoolUnattributedBytes -ne ($report.rendererMeshRecordPoolLiveBytes-$recordBytes) -or
                    $report.rendererMeshRecordClosureComplete -ne (($report.rendererMeshLiveRecords -eq $report.rendererMeshFactsPresent) -and $report.rendererMeshPoolUnattributedBytes -eq 0)) {
                    throw 'Same-cut mesh record and logical pool residual evidence was inconsistent'
                }
                if ($MeshAck) {
                    if (!$report.rendererMeshAckEnabled -or !$report.rendererMeshAckValid -or $report.rendererMeshAckState -ne 3 -or
                        $report.rendererMeshAckTicket -lt 1 -or $report.rendererMeshAckEpoch -ne $report.rendererMeshFactEpoch -or
                        $report.rendererMeshAckSubmission -lt 1 -or $report.rendererMeshAckPoolGeneration -lt 1 -or
                        $report.rendererMeshAckRequested -ne $report.rendererMeshFactsRequested -or
                        $report.rendererMeshAckPresent -ne $report.rendererMeshFactsPresent -or $report.rendererMeshAckAbsent -ne $report.rendererMeshFactsAbsent -or
                        $report.rendererMeshAckVertexBytes -ne $report.rendererMeshFactVertexBytes -or $report.rendererMeshAckIndexBytes -ne $report.rendererMeshFactIndexBytes -or
                        $report.rendererMeshAckPoolGeneration -ne $report.rendererMeshRecordPoolGeneration -or
                        $report.rendererMeshAckPoolLiveBytes -ne $report.rendererMeshRecordPoolLiveBytes -or
                        ($report.rendererMeshAckVertexBytes + $report.rendererMeshAckIndexBytes) -gt $report.rendererMeshAckPoolLiveBytes) {
                        throw 'Actual immutable mesh cut did not receive its bounded asynchronous queue acknowledgement'
                    }
                } elseif ($report.rendererMeshAckEnabled -or $report.rendererMeshAckTicket -ne 0 -or $report.rendererMeshAckState -ne 0) {
                    throw 'Owner-only run unexpectedly enabled queue acknowledgement'
                }
            } elseif ($report.persistentAttributionEnabled) {
                throw 'Default diagnostic unexpectedly enabled persistent owner attribution'
            } elseif ($report.rendererMeshFactsEnabled -or $report.rendererMeshFactCalls -ne 0 -or $report.rendererMeshFactsRequested -ne 0) {
                throw 'Default-off run unexpectedly queried renderer handle facts'
            }
            if (!$MeshAck -and ($report.rendererMeshAckEnabled -or $report.rendererMeshAckTicket -ne 0 -or $report.rendererMeshAckState -ne 0)) {
                throw 'Default-off run unexpectedly acquired a mesh queue acknowledgement ticket'
            }
            if ($RegisteredRefs) {
                if (!$report.rendererRegisteredRefsEnabled -or !$report.rendererRegisteredRefsValid -or !$report.rendererRegisteredRefsComplete -or
                    $report.rendererRegisteredRefsCalls -ne 1 -or $report.rendererRegisteredRefsRows -ne $report.rendererMeshFactsRequested -or
                    $report.rendererRegisteredRefsRows -gt 8192 -or $report.rendererRegisteredModelsVisited -gt 8192 -or
                    $report.rendererRegisteredLodsVisited -gt 262144 -or $report.rendererRegisteredSectionsVisited -gt 1048576 -or
                    $report.rendererRegisteredRefusalFlags -ne 0 -or $report.rendererRegisteredMissingRecords -ne 0 -or
                    $report.rendererRegisteredReferencedMeshes -lt 1 -or $report.rendererRegisteredModelRefs -lt 1 -or
                    $report.rendererRegisteredModelLodRefs -lt $report.rendererRegisteredModelRefs -or
                    $report.rendererRegisteredSectionOccurrences -lt $report.rendererRegisteredModelLodRefs -or
                    $report.rendererRegisteredSectionOccurrences -gt $report.rendererRegisteredSectionsVisited) {
                    throw 'Actual registered-section census is incomplete, invalid or not exercised'
                }
                $referenceSamples = @($report.rendererRegisteredRefSamples)
                if (!$referenceSamples.Count -or $referenceSamples.Count -gt 8) { throw 'Registered reference sample bound failed' }
                foreach ($reference in $referenceSamples) {
                    if ([uint64]$reference.mesh -eq 0 -or $reference.models -lt 1 -or $reference.modelLods -lt $reference.models -or
                        $reference.sections -lt $reference.modelLods -or $reference.flags -ne 0) { throw 'Registered full-handle sample is inconsistent' }
                }
            } elseif ($report.rendererRegisteredRefsEnabled -or $report.rendererRegisteredRefsCalls -ne 0 -or
                $report.rendererRegisteredRefsRows -ne 0 -or @($report.rendererRegisteredRefSamples).Count) {
                throw 'Default-off run unexpectedly scanned registered section references'
            }
            if ($report.registryEntriesVisited -gt $report.maxModels -or
                ($report.modelsRequested + $report.ineligibleEntriesSkipped) -ne $report.registryEntriesVisited) { throw 'Registry sampling and negative-cache accounting disagree' }
            if ($report.missingModels -ne 0 -or $report.missingMeshes -ne 0) { throw 'Settled eligible retained models or meshes are missing' }
            return $report
        }
    $initialCold = Wait-Settled 'initial-cold'; $initialQuiet = Wait-Quiet 'initial-quiet'
    $returns = @(); $returnAllocations = @()
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
        if ($AllocationReport -and $cycle -ge 2) {
            # A new bounded owner/renderer cut AFTER the quiet sample and route timer.
            # It is not atomic with that preceding memory sample and adds an inter-route interval.
            $cutStart = [DateTime]::UtcNow.ToString('o')
            $returnReport = Read-Allocation @{cmd='geometry_allocations'}
            $returnReport | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out ("geometry-allocations-return-$cycle.json"))
            $returnAllocations += [pscustomobject]@{cycle=$cycle; quietSampleUtc=$quiet.last.utc
                requestedUtc=$cutStart; completedUtc=[DateTime]::UtcNow.ToString('o'); report=$returnReport}
        }
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
    $returnAllocationComparison = $null
    if ($returnAllocations.Count -eq 2) {
        $secondReport=$returnAllocations[0].report; $thirdReport=$returnAllocations[1].report
        $numericFields=@('uniqueVertexBytes','uniqueIndexBytes','poolLiveBytes',
            'persistentCpuReferencedVertexBytes','persistentCpuReferencedIndexBytes',
            'persistentRetainedSharedCpuVertexBytes','persistentRetainedSharedCpuIndexBytes',
            'persistentStandaloneCpuVertexBytes','persistentStandaloneCpuIndexBytes',
            'persistentTrackedAllocations','persistentCpuBorrowers','persistentModelLodLinks',
            'rendererMeshLiveRecords','rendererMeshRecordPoolLiveBytes','rendererMeshPoolUnattributedBytes')
        $numericDeltas=@()
        foreach($field in $numericFields) {
            if($null -ne $secondReport.$field -and $null -ne $thirdReport.$field) {
                $numericDeltas += [pscustomobject]@{field=$field; return2=$secondReport.$field; return3=$thirdReport.$field
                    delta=([decimal]$thirdReport.$field-[decimal]$secondReport.$field)}
            }
        }
        # Exact asset strings and LOD ordinals identify reported rows, not physical mesh unions.
        # Multiple registrations/LODs can overlap; unordered registry sampling can also change.
        $rowSets=@()
        foreach($report in @($secondReport,$thirdReport)) {
            $set=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
            foreach($row in @($report.rows)) {
                foreach($field in @('asset','lodIndex','vertexBytes','indexBytes')) {
                    if($null -eq $row.$field) { throw "Per-return allocation row omitted $field" }
                }
                $key=(@([string]$row.asset,[int]$row.lodIndex) | ConvertTo-Json -Compress)
                if(!$set.ContainsKey($key)) {
                    $set.Add($key,[pscustomobject]@{asset=$row.asset; lodIndex=$row.lodIndex; occurrences=0; vertexBytes=[decimal]0; indexBytes=[decimal]0})
                }
                $entry=$set[$key]; $entry.occurrences++; $entry.vertexBytes += [decimal]$row.vertexBytes; $entry.indexBytes += [decimal]$row.indexBytes
            }
            $rowSets += ,$set
        }
        $keys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach($set in $rowSets) { foreach($key in $set.Keys) { $null=$keys.Add($key) } }
        $rowDeltas=@(foreach($key in $keys) {
            $left=$null; $right=$null
            $null=$rowSets[0].TryGetValue($key,[ref]$left); $null=$rowSets[1].TryGetValue($key,[ref]$right)
            $identity=if($null -ne $right){$right}else{$left}
            [pscustomobject]@{asset=$identity.asset; lodIndex=$identity.lodIndex; return2=$left; return3=$right}
        })
        $returnAllocationComparison=[pscustomobject]@{
            scope='Separate post-quiet diagnostic cuts outside route timings; no acceptance override. Asset+LOD rows are sampled overlapping occurrence totals, not physical mesh unions or selected demand.'
            eligibleReportedRowsComplete=(!$secondReport.truncated -and !$thirdReport.truncated)
            return2Truncated=[bool]$secondReport.truncated; return3Truncated=[bool]$thirdReport.truncated
            cuts=@($returnAllocations | Select-Object cycle,quietSampleUtc,requestedUtc,completedUtc)
            numericDeltas=$numericDeltas; assetLodRows=@($rowDeltas | Sort-Object asset,lodIndex)
        }
        $returnAllocationComparison | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-allocations-return-comparison.json')
    }
    $allocation = $null
    if ($AllocationReport) {
        # Diagnostics run after timed routes and quiet samples; they do not prove usage or reclaimability.
        $allocation = Read-Allocation @{cmd='geometry_allocations'} -finalEvidence $true
        if (@($allocation.rows).Count -lt 2 -or $allocation.uniqueMeshes -lt 1) { throw 'No meaningful retained geometry allocation rows' }
        $boundedReport = Read-Allocation @{cmd='geometry_allocations'; maxModels=1; maxRows=1; maxSectionVisits=65536} -finalEvidence $true
        if (!$boundedReport.truncated -or @($boundedReport.rows).Count -gt 1) { throw 'Bounded report truncation was not preserved' }
        $stale = Send @{cmd='geometry_allocations'; requestId=$allocation.requestId}
        if ($stale.status -ne 'Invalid') { throw 'Superseded report ID remained claimable' }
        $allocation | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-allocations.json')
        $boundedReport | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-allocations-bounded.json')
        if ($LodDemandReport) {
            # Row bytes are a selection heuristic, not exclusive/reclaimable bytes.
            $modelIds = @($allocation.rows | Sort-Object { $_.vertexBytes + $_.indexBytes } -Descending |
                Select-Object -ExpandProperty rendererModel -Unique | Select-Object -First 8)
            if (!$modelIds.Count) { throw 'No retained renderer models for LOD demand observation' }
            function Wait-LodDemand($id) {
                $deadline = [DateTime]::UtcNow.AddSeconds(30)
                do {
                    Start-Sleep -Milliseconds 100
                    $report = Send @{cmd='stream_lod_demand'; action='poll'; requestId=$id}
                    if ([DateTime]::UtcNow -gt $deadline) { throw 'Bounded LOD demand observation did not complete asynchronously' }
                } while ($report.status -in @('Pending', 'Draining'))
                return $report
            }
            $first = Send @{cmd='stream_lod_demand'; action='request'; modelIds=$modelIds; frames=64}
            if ($first.status -ne 'Pending' -or $first.requestId -lt 1) { throw 'LOD demand request was not accepted' }
            $busy = Send @{cmd='stream_lod_demand'; action='request'; modelIds=$modelIds; frames=64}
            if ($busy.status -ne 'Busy') { throw 'Concurrent LOD demand request replaced an active epoch' }
            $cancel = Send @{cmd='stream_lod_demand'; action='cancel'; requestId=$first.requestId}
            $cancelled = Wait-LodDemand $first.requestId
            if ($cancelled.status -ne 'Cancelled') { throw 'LOD demand cancellation did not retire its epoch' }
            $second = Send @{cmd='stream_lod_demand'; action='request'; modelIds=$modelIds; frames=16}
            if ($second.status -ne 'Pending' -or $second.requestId -le $first.requestId) { throw 'Replacement LOD demand epoch did not advance' }
            $staleDemand = Send @{cmd='stream_lod_demand'; action='poll'; requestId=$first.requestId}
            if ($staleDemand.status -ne 'Invalid') { throw 'Superseded LOD demand remained claimable' }
            $demand = Wait-LodDemand $second.requestId
            if ($demand.status -ne 'Ready' -or @($demand.rows).Count -ne $modelIds.Count) { throw 'Selected retained LOD demand response incomplete' }
            if ($demand.framesSampled -lt 1 -or $demand.passMask -eq 0 -or !@($demand.rows | Where-Object { $_.lodMask -ne 0 }).Count) { throw 'No actual dispatched GPU LOD feedback observed for the selected models' }
            if (!$demand.PSObject.Properties['ownershipCoverageComplete'] -or !$demand.PSObject.Properties['cachedViewCoverageComplete'] -or
                $demand.ownershipCoverageComplete -or $demand.cachedViewCoverageComplete) { throw 'Dispatched-pass diagnostic omitted limitations or claimed complete geometry ownership or cached-view usage' }
            foreach ($row in $demand.rows) {
                if ($row.rendererModel -notin $modelIds -or $row.state -ne 'Live' -or $row.lodCount -lt 1 -or $row.lodCount -gt 32) { throw 'Actual retained model identity or LOD bounds changed during observation' }
                if ($row.lodCount -lt 32 -and [uint64]$row.lodMask -ge ([uint64]1 -shl $row.lodCount)) { throw 'LOD demand mask exceeded the actual model LOD count' }
            }
            @{selectionScope='Largest inspected rows; not exclusive bytes'; modelIds=$modelIds; first=$first; busy=$busy; cancel=$cancel; cancelled=$cancelled; second=$second; stale=$staleDemand; demand=$demand} |
                ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'geometry-lod-demand.json')
        }
    }
    $parkedPositive = !$ParkedGeometryReport -or @($script:parkedReportObservations | Where-Object {
        $_.parkedModelsCaptured -gt 0 -and $_.parkedUniqueAllocations -gt 0
    }).Count -gt 0
    @{requested=[bool]$ParkedGeometryReport; positive=[bool]($ParkedGeometryReport -and $parkedPositive)
        observations=$script:parkedReportObservations
        scope='Bounded earlier producer parked selection joined to later renderer facts; diagnostic usage only, incomplete classes remain Unknown; no reclaimability or performance acceptance'} |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'parked-candidate-proof.json')
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'returned-settled.png')}
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) { throw 'Game failed to exit cleanly' }
    if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Renderer error' }
    # Publish verdict only after the owned process exits cleanly. Process/driver
    # measurements remain raw evidence; resident count alone proves no memory bound.
    @{passed = (($converged -or $DiagnosticOnly) -and $parkedPositive); convergenceMeasured = !$DiagnosticOnly; status = $status; initialCold = $initialCold; initialQuiet = $initialQuiet
        returns = $returns; comparison = $comparison; lateQuiet = $lateQuiet; retiredClear = $retiredClear
        processScope = 'Diagnostic private/RSS samples retained; GPU convergence does not establish process-memory convergence'
        returnAllocationComparison = $returnAllocationComparison
        allocationReport = $allocation; lodDemandMeasured = [bool]$LodDemandReport; ownerLedgerMeasured = [bool]$OwnerLedger; meshAckMeasured = [bool]$MeshAck
        registeredRefsMeasured = [bool]$RegisteredRefs
        parkedGeometryReportMeasured = [bool]$ParkedGeometryReport; parkedCandidateUsagePositive = [bool]($ParkedGeometryReport -and $parkedPositive)
        pressureCacheTrim = [bool]$PressureCacheTrim
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
    Write-Output "Resource convergence evidence: $out ($status)"
    if (!$parkedPositive) { throw 'NotExercised: parked selection did not capture actual model and allocation candidates; evidence retained' }
    if (!$converged -and !$DiagnosticOnly) { throw "Convergence not established: $status (see result.json and raw samples)" }
} finally {
    if ($gpuSampler -and !$gpuSampler.HasExited) { Stop-Process -Id $gpuSampler.Id }
    if ($p -and !$p.HasExited) {
        # Verification failures still allow normal cleanup of this owned session.
        try {
            if ($client -and $client.Connected) {
                $null = Send @{cmd = 'exit'}
                $null = $p.WaitForExit(15000)
            }
        } catch {}
        if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    if ($client) { $client.Dispose() }
    foreach ($name in $changedNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
