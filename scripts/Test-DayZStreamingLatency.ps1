param(
    [ValidateSet('Disabled', 'Cold')][string]$Mode = 'Disabled',
    [switch]$TerrainPageTimings,
    [switch]$WarmProfile,
    [ValidateSet('Counted', 'Timed')][string]$AdmissionMode = 'Counted',
    [switch]$ParkedTextureRefill,
    [switch]$LazyTextureGroups,
    [switch]$PaaLzoReplay,
    [switch]$PaaPreparationDiagnostics,
    [switch]$PackedTextures,
    [switch]$PaaPreparationInflight,
    [switch]$RegistrationQuota,
    [switch]$ArchiveSourceBindings,
    [switch]$WarmTextureJobs,
    [switch]$RequireWarmMaterialStageUpload,
    [switch]$LegacySectionRefresh,
    [switch]$SectionReuse,
    [switch]$LodReuse,
    [switch]$ModelRowUpload,
    [switch]$GeometryCopyDiagnostics,
    [switch]$ColdPaaHandoff,
    [switch]$RequireColdAlphaPeek,
    [string]$Label = 'dayz-streaming-latency',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if ($RequireColdAlphaPeek) { $ColdPaaHandoff = $true }
if ($RequireWarmMaterialStageUpload) { $WarmTextureJobs = $true; $WarmProfile = $true }
if ($WarmTextureJobs) { $ArchiveSourceBindings = $true }
if ($RegistrationQuota -and $AdmissionMode -ne 'Counted') { throw 'Registration quota requires counted admission' }
if ($PaaPreparationDiagnostics -or $PaaPreparationInflight) { $PackedTextures = $true }
if (!$env:LOCK_OWNER) {
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $env:LOCK_OWNER = 'DayZ bounded streaming latency screening'
    try {
        $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe, '-NoProfile', '-File', $PSCommandPath, '-Mode', $Mode, '-AdmissionMode', $AdmissionMode, '-Label', $Label, '-GameDir', $GameDir)
        if ($TerrainPageTimings) { $argv += '-TerrainPageTimings' }
        if ($WarmProfile) { $argv += '-WarmProfile' }
        if ($ParkedTextureRefill) { $argv += '-ParkedTextureRefill' }
        if ($LazyTextureGroups) { $argv += '-LazyTextureGroups' }
        if ($PaaLzoReplay) { $argv += '-PaaLzoReplay' }
        if ($PaaPreparationDiagnostics) { $argv += '-PaaPreparationDiagnostics' }
        if ($PackedTextures) { $argv += '-PackedTextures' }
        if ($PaaPreparationInflight) { $argv += '-PaaPreparationInflight' }
        if ($RegistrationQuota) { $argv += '-RegistrationQuota' }
        if ($ArchiveSourceBindings) { $argv += '-ArchiveSourceBindings' }
        if ($WarmTextureJobs) { $argv += '-WarmTextureJobs' }
        if ($RequireWarmMaterialStageUpload) { $argv += '-RequireWarmMaterialStageUpload' }
        if ($LegacySectionRefresh) { $argv += '-LegacySectionRefresh' }
        if ($SectionReuse) { $argv += '-SectionReuse' }
        if ($LodReuse) { $argv += '-LodReuse' }
        if ($ModelRowUpload) { $argv += '-ModelRowUpload' }
        if ($GeometryCopyDiagnostics) { $argv += '-GeometryCopyDiagnostics' }
        if ($ColdPaaHandoff) { $argv += '-ColdPaaHandoff' }
        if ($RequireColdAlphaPeek) { $argv += '-RequireColdAlphaPeek' }
        & 'C:\Program Files\Git\bin\bash.exe' @argv
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
    # Texture admission is independent of Mode's simulation-only Cold controls.
    WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF = $(if ($ColdPaaHandoff) { '1' } else { '0' })
    WGR_NATIVE_DDS_PREPARE = '0'; WGR_NATIVE_DDS_BC3_ONLY = '0'
    WGR_ADAPTIVE_TEXTURE_DETAIL = '0'; WGR_LOD_GOVERNOR_RANGE = '1'; WGR_SKY_VOLUME_INCREMENTAL = '1'
    WGR_TEXTURE_STREAM_STATS = '0'; WGR_RENDER_CALL_CPU_TIMINGS = '0'
    WGR_CULL_SECTION_REFRESH = $(if ($LegacySectionRefresh) { '0' } else { '1' })
    WGR_CULL_SECTION_REUSE = $(if ($SectionReuse) { '1' } else { '0' })
    WGR_CULL_LOD_REUSE = $(if ($LodReuse) { '1' } else { '0' })
    WGR_CULL_MODEL_ROW_UPLOAD = $(if ($ModelRowUpload) { '1' } else { '0' })
    WGR_GEO_POOL_COPY_DIAGNOSTICS = $(if ($GeometryCopyDiagnostics) { '1' } else { '0' })
    WGR_WATER_BACKEND = '0'
}
if ($TerrainPageTimings) {
    $environment.WGR_TERRAIN_PAGE_TIMINGS = '1'
    $environment.WGR_RENDER_CALL_CPU_TIMINGS = '1'
    $environment.POSEIDON_SLOW_FRAME_TRACE = '1'
}
if ($WarmProfile) { $environment.WGR_OBJECT_STREAM_WARM_PROFILE = '1' }
if ($AdmissionMode -eq 'Timed') { $environment.WGR_OBJECT_STREAM_ADMIT = 'timed' }
if ($ParkedTextureRefill) {
    $environment.WGR_GPU_MODEL_PARK_REFILL = '1'
    $environment.WGR_MODEL_RESIDENCY_TRACE = '1'
}
if ($LazyTextureGroups) { $environment.WGR_LAZY_TEXTURE_BIND_GROUPS = '1' }
if ($PaaLzoReplay) { $environment.WGR_PAA_LZO_REPLAY_CACHE = '1' }
if ($PackedTextures) { $environment.WGR_OBJECT_STREAM_PBO_TEXTURES = '1' }
if ($PaaPreparationInflight) { $environment.WGR_PAA_PREP_INFLIGHT = '1' }
if ($RegistrationQuota) { $environment.WGR_OBJECT_STREAM_REGISTRATION_QUOTA = '1' }
if ($ArchiveSourceBindings) { $environment.WGR_OBJECT_STREAM_WARM_TEXTURES = '1' }
if ($WarmTextureJobs) { $environment.WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS = '1' }
if ($PaaPreparationDiagnostics) {
    $environment.WGR_PAA_PREP_ATTEMPT_DIAG = '1'
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
        paaLzoReplay = [bool]$PaaLzoReplay
        paaPreparationDiagnostics = [bool]$PaaPreparationDiagnostics
        packedTextures = [bool]$PackedTextures; paaPreparationInflight = [bool]$PaaPreparationInflight
        registrationQuota = [bool]$RegistrationQuota
        archiveSourceBindings = [bool]$ArchiveSourceBindings
        coldPaaHandoff = [bool]$ColdPaaHandoff; requireColdAlphaPeek = [bool]$RequireColdAlphaPeek
        coldPaaHandoffEnvironment = [string]$environment.WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF
        warmTextureJobs = [bool]$WarmTextureJobs
        requireWarmMaterialStageUpload = [bool]$RequireWarmMaterialStageUpload
        renderCallCpuTimings = [bool]$TerrainPageTimings
        renderCallCpuTimingEnvironment = [string]$environment.WGR_RENDER_CALL_CPU_TIMINGS
        modelRowUpload = [bool]$ModelRowUpload; modelRowUploadEnvironment = [string]$environment.WGR_CULL_MODEL_ROW_UPLOAD
        legacySectionRefresh = [bool]$LegacySectionRefresh
        cullSectionRefreshEnvironment = [string]$environment.WGR_CULL_SECTION_REFRESH
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
        if ($null -eq $camera.coldAlphaPeeks -or $camera.coldAlphaPeeks -lt 0 -or
            [double]::IsNaN([double]$camera.coldAlphaPeeks) -or [double]::IsInfinity([double]$camera.coldAlphaPeeks) -or
            $camera.coldAlphaPeeks -ne [math]::Truncate([double]$camera.coldAlphaPeeks)) { throw 'Missing or invalid cold alpha commit counter' }
        if (!$ColdPaaHandoff -and $camera.coldAlphaPeeks -ne 0) { throw 'OFF performed a cold owned alpha peek' }
        if ($Mode -eq 'Disabled' -and ($simulation.status -ne 'Disabled' -or $simulation.positiveColdRequested -ne 0 -or $simulation.positiveColdCreated -ne 0)) { throw 'Disabled mode performed simulation residency or cold admission' }
        if ($Mode -eq 'Cold' -and (($simulation.metadataComplete -and $simulation.status -ne 'CapacityExceeded') -or $simulation.readyRegions -ne 0 -or $simulation.readyQueries -ne 0)) { throw 'Partial production service fabricated full readiness' }
        foreach ($field in @('warmTextureJobsEnabled','warmTextureQueued','warmTextureActive',
            'warmTextureMetadataBytesPeak','warmTextureQueueBytes','warmTextureScratchBytesPeak')) {
            if ($null -eq $camera.$field) { throw "Missing warm preparation bound field: $field" }
        }
        if ($camera.warmTextureJobsEnabled -ne [bool]$WarmTextureJobs -or
            ($camera.warmTextureQueued + $camera.warmTextureActive) -gt 8 -or
            $camera.warmTextureMetadataBytesPeak -gt 2097152 -or
            $camera.warmTextureQueueBytes -gt 2097152 -or
            $camera.warmTextureScratchBytesPeak -gt (2 * (67108864 + 65536))) { throw 'Warm preparation flag or bounded work/storage invariant failed' }
        if (!$WarmTextureJobs) {
            foreach ($field in @('warmTextureJobsSubmitted','warmTextureJobsCompleted','warmTextureJobsCancelled',
                'warmTextureJobsCoalesced','warmTextureJobsRejected','warmTextureMembersPrepared','warmTextureMembersSkipped',
                'warmTextureMembersUnreadable','warmTextureQueued','warmTextureActive','warmTextureMetadataBytes',
                'warmTextureMetadataBytesPeak','warmTextureScratchBytes','warmTextureScratchBytesPeak',
                'warmTextureQueueBytes','warmTextureMs','warmPreparedPuts','warmPreparedTakes','warmPreparedSourceRefused',
                'warmPreparedUploads','warmPreparedUploadBytes','warmPreparedUploadFailed')) {
                if ($null -eq $camera.$field -or $camera.$field -ne 0) { throw "Disabled warm preparation did work or allocated queue storage: $field" }
            }
        }
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
    $warmJobProof = @{enabled = [bool]$WarmTextureJobs; state = $returned.camera
        scope = 'Bounded requested dependencies of cached CPU models; successful GPU block uploads validate initialized physical member and current loaded mount, not whole-model texture readiness or a performance benefit'}
    $warmJobProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'warm-texture-job-proof.json')
    $warmJobsProven = !$WarmTextureJobs -or ($returned.camera.warmTextureJobsSubmitted -ge 1 -and
        $returned.camera.warmTextureMembersPrepared -ge 1 -and $returned.camera.warmPreparedPuts -ge 1 -and
        $returned.camera.warmPreparedTakes -ge 1 -and $returned.camera.warmPreparedUploads -ge 1 -and
        $returned.camera.warmPreparedUploadBytes -ge 1)
    $archiveBindings = Send @{cmd = 'archive_source_bindings'}
    if ($archiveBindings.enabled -ne [bool]$ArchiveSourceBindings -or
        $archiveBindings.liveBindings -gt $archiveBindings.capBindings -or
        $archiveBindings.knownCppBytes -gt $archiveBindings.capBytes) { throw 'Actual-read source binding state or cap mismatch' }
    if ($ArchiveSourceBindings) {
        if ($archiveBindings.capBindings -ne 256 -or $archiveBindings.capBytes -ne 2097152 -or
            $archiveBindings.wrappedReads -lt 1 -or $archiveBindings.initRetains -lt 1 -or
            $archiveBindings.liveBindings -lt 1) { throw 'Mapped archive provenance did not reach an initialized texture source within its bounds' }
    } elseif ($archiveBindings.liveBindings -ne 0 -or $archiveBindings.knownCppBytes -ne 0 -or
        $archiveBindings.wrappedReads -ne 0 -or $archiveBindings.initRetains -ne 0 -or
        $archiveBindings.captureRefused -ne 0 -or $archiveBindings.capacityRefused -ne 0 -or
        $archiveBindings.allocationRefused -ne 0) { throw 'Default path unexpectedly captured archive source provenance' }
    $archiveBindings | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'archive-source-binding-proof.json')
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'returned-settled.png')}
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) { throw 'Owned game failed to exit cleanly' }
    if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Renderer error' }
    if ($WarmProfile -and !(Select-String -LiteralPath $log -Pattern 'Object stream warm update profile: row=' -Quiet)) { throw 'Warm admission profile did not observe a measured update' }
    # Filename family does NOT establish an actual shader/material-slot role.
    # Emitted only after current-source/layout validation and successful worker-chain upload.
    $coldUploadRows = @(Select-String -LiteralPath $log -SimpleMatch 'Cold PAA handoff upload:')
    $coldUploadMatches = @(Select-String -LiteralPath $log -Pattern 'Cold PAA handoff upload: source=(.+?) bytes=(\d+) sourceValidated=true blockUploadSucceeded=true operationLocal=true row=(\d+) limit=16')
    if ($coldUploadRows.Count -ne $coldUploadMatches.Count -or $coldUploadMatches.Count -gt 16) { throw 'Malformed or unbounded cold owned upload witness' }
    $coldUploadObservations = @($coldUploadMatches | ForEach-Object {
        $match=$_.Matches[0];$source=[string]$match.Groups[1].Value
        $bytes=[uint64]$match.Groups[2].Value;$row=[uint64]$match.Groups[3].Value
        if (!$source.Length -or $source.Length -gt 8191 -or !$bytes -or $bytes -gt 16777216 -or !$row -or $row -gt 16) { throw 'Invalid cold owned upload witness fields' }
        @{source=$source;bytes=$bytes;row=$row;sourceValidated=$true;blockUploadSucceeded=$true;operationLocal=$true}
    })
    if (!$ColdPaaHandoff -and $coldUploadObservations.Count) { throw 'Cold handoff upload occurred in OFF control' }
    $coldPeekDelta = $returned.camera.coldAlphaPeeks - $baseline.camera.coldAlphaPeeks
    if ($coldPeekDelta -lt 0) { throw 'Cold alpha cumulative counter went backwards' }
    $coldAlphaProven = !$RequireColdAlphaPeek -or ($coldPeekDelta -gt 0 -and $coldUploadObservations.Count -gt 0)
    $coldPaaProof = @{requested=[bool]$ColdPaaHandoff;requireAlphaPeek=[bool]$RequireColdAlphaPeek
        environment=[string]$environment.WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF
        baselinePeeks=$baseline.camera.coldAlphaPeeks;returnedPeeks=$returned.camera.coldAlphaPeeks;routePeekDelta=$coldPeekDelta
        positive=[bool]($coldPeekDelta -gt 0 -and $coldUploadObservations.Count -gt 0);requirementSatisfied=[bool]$coldAlphaProven;uploadObservations=$coldUploadObservations
        truncated=[bool](Select-String -LiteralPath $log -SimpleMatch 'Cold PAA handoff upload truncated:' -Quiet)
        scope='Actual route alpha commit delta plus actual operation-local source-validated owned-chain upload; separate witnesses need not name the same texture. Not byte/pixel parity, whole-route totals, GPU completion or performance acceptance'}
    $coldPaaProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'cold-owned-alpha-proof.json')
    $warmUploadRows = @(Select-String -LiteralPath $log -Pattern 'Warm prepared upload witness: source=(.*?) bytes=(\d+) sourceMemberBytes=(\d+) sourceValidated=true blockUploadSucceeded=true row=(\d+) limit=64')
    $warmUploadObservations = @($warmUploadRows | ForEach-Object {
        $g = $_.Matches[0].Groups
        [pscustomobject]@{ source = [string]$g[1].Value; bytes = [uint64]$g[2].Value
            sourceMemberBytes = [uint64]$g[3].Value; row = [uint64]$g[4].Value
            stageFamily = [bool]($g[1].Value -match '(?i)_(nohq|smdi|dtsmdi)\.paa$') }
    })
    if (@($warmUploadObservations | Where-Object { $_.bytes -eq 0 -or $_.sourceMemberBytes -eq 0 -or
        $_.sourceMemberBytes -gt 16777216 -or $_.row -lt 1 -or $_.row -gt 64 }).Count -gt 0 -or
        $warmUploadObservations.Count -gt 64) { throw 'Malformed bounded warm-upload witness' }
    if ($warmUploadObservations.Count -gt 0 -and (!$WarmTextureJobs -or !$WarmProfile)) { throw 'Warm-upload witness unexpectedly active outside requested flags' }
    $warmStageFamilyObserved = @($warmUploadObservations | Where-Object { $_.stageFamily }).Count -gt 0
    $warmStageFamilyProven = !$RequireWarmMaterialStageUpload -or ($warmJobsProven -and $warmStageFamilyObserved)
    $warmStageFamilyProof = @{ required = [bool]$RequireWarmMaterialStageUpload; positive = [bool]$warmStageFamilyObserved
        observations = $warmUploadObservations
        truncated = [bool](Select-String -LiteralPath $log -Pattern 'Warm prepared upload witness truncated:' -Quiet)
        scope = 'Successful source-validated bound worker upload of normal/specular filename family only; not shader slot role, visual quality, whole-model readiness or performance acceptance' }
    $warmStageFamilyProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'warm-material-stage-family-proof.json')
    $replayProof = $null
    $replayRows = @(Select-String -LiteralPath $log -Pattern 'PAA LZO replay: hits=(\d+) misses=(\d+) hitOutputBytes=(\d+) storeBytes=(\d+) peakStoreBytes=(\d+) entries=(\d+) evictions=(\d+) oversize=(\d+) admissionFailures=(\d+) budgetBytes=33554432 maxEntries=128')
    if ($PaaLzoReplay) {
        $observed = @($replayRows | ForEach-Object {
            $g = $_.Matches[0].Groups
            [pscustomobject]@{hits = [uint64]$g[1].Value; misses = [uint64]$g[2].Value; hitOutputBytes = [uint64]$g[3].Value
                storeBytes = [uint64]$g[4].Value; peakStoreBytes = [uint64]$g[5].Value; entries = [uint64]$g[6].Value
                evictions = [uint64]$g[7].Value; oversize = [uint64]$g[8].Value; admissionFailures = [uint64]$g[9].Value}
        })
        $positiveHits = @($observed | Where-Object { $_.hits -gt 0 -and $_.hitOutputBytes -gt 0 }).Count -gt 0
        $withinBounds = $observed.Count -gt 0 -and @($observed | Where-Object { $_.storeBytes -gt 33554432 -or $_.peakStoreBytes -gt 33554432 -or $_.entries -gt 128 }).Count -eq 0
        $replayProof = @{positiveHits = $positiveHits; withinBounds = $withinBounds; observations = $observed
            truncated = [bool](Select-String -LiteralPath $log -Pattern 'PAA LZO replay stats truncated:' -Quiet)
            scope = 'Bounded cumulative observations of exact compressed-byte replay; retained logical bytes include fixed metadata, exclude caller buffers and allocator overhead. No RSS cap or performance acceptance.'}
        $replayProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'paa-lzo-replay-proof.json')
        if (!$positiveHits -or !$withinBounds) { throw 'PAA replay lacked actual positive hits or bounded retained-storage proof' }
    } elseif ($replayRows.Count) { throw 'Default-off run unexpectedly enabled PAA replay' }
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
    # Post-exit proof from existing slow rows; no sampled-route command or renderer work.
    # No positive paired row means NotExercised, never diagnostic/performance success.
    $callCpuRows = @(Select-String -LiteralPath $log -Pattern 'Wgpu slow block: .*callCpuEnabled=(\d+) callCpuToken=(\d+) callCpuState=(\d+) callCpuCount=(\d+) callCpuPaired=(\d+) renderStatus=(-?\d+)')
    $pairedCpuRows = @()
    foreach ($row in $callCpuRows) {
        $m = $row.Matches[0]
        $enabled = [uint32]$m.Groups[1].Value; $token = [uint64]$m.Groups[2].Value
        $state = [uint32]$m.Groups[3].Value; $bucketCount = [uint32]$m.Groups[4].Value
        $paired = [uint32]$m.Groups[5].Value; $status = [int]$m.Groups[6].Value
        if ($enabled -ne [uint32][bool]$TerrainPageTimings -or $paired -gt 1) { throw 'Render-call timing startup gate disagrees with provenance' }
        if ($paired -eq 1) {
            if ($enabled -ne 1 -or !$TerrainPageTimings -or $token -eq 0 -or $state -ne 1 -or $bucketCount -ne 12 -or $status -ne 0) { throw 'Render-call CPU timing pairing metadata invalid' }
            foreach ($field in @('acquireCpu','presentCpu','setupInclusiveCpu','submitCpu','harvestCpu','encEarlyCpu','encShadowCpu','encMainInclusiveCpu','encTailCpu','encSkyCpu','encDrawCpu','encPostCpu')) {
                $values = [regex]::Matches($row.Line, ('(?:^|\s)' + $field + '=(-?\d+(?:\.\d+)?)'))
                if ($values.Count -ne 1) { throw "Render-call CPU timing bucket missing/duplicated: $field" }
                $value = [double]::Parse($values[0].Groups[1].Value, $culture)
                if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or ($value -lt 0 -and $value -ne -1)) { throw "Render-call CPU timing bucket invalid: $field" }
            }
            $pairedCpuRows += $row.Line
        } elseif (!$TerrainPageTimings -and ($token -ne 0 -or $state -ne 0 -or $bucketCount -ne 0)) {
            throw 'Default run unexpectedly collected render-call CPU timing data'
        }
    }
    $callCpuProof = @{enabled = [bool]$TerrainPageTimings; slowRows = $callCpuRows.Count; pairedRows = $pairedCpuRows.Count
        rows = @($callCpuRows | ForEach-Object { $_.Line })
        scope = 'Exact observed render-call CPU buckets only; hierarchy overlaps, not GPU elapsed/submission completion, frame percentiles or performance acceptance'}
    $callCpuProof | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'render-call-cpu-proof.json')
    if ($TerrainPageTimings -and $pairedCpuRows.Count -lt 1) { throw 'Render-call CPU timing was not exercised by a completed paired slow row; evidence retained' }
    $parkedProof = $null
    if ($ParkedTextureRefill) {
        $deferredRows = @(Select-String -LiteralPath $log -Pattern 'Wgpu parked refill deferred: models=([1-9]\d*)')
        $reusedRows = @(Select-String -LiteralPath $log -Pattern 'Wgpu parked refill activation: .* images=([1-9]\d*) reused=true failed=false')
        $parkedProof = @{deferredObserved = $deferredRows.Count -gt 0; activationReuseObserved = $reusedRows.Count -gt 0
            deferredRows = @($deferredRows | ForEach-Object { $_.Line }); reusedRows = @($reusedRows | ForEach-Object { $_.Line })
            scope = 'Repeated parked observations are not unique model counts; successful leased-slot activation only, not memory convergence or performance acceptance'}
        $parkedProof | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'parked-refill-proof.json')
        if (!$parkedProof.deferredObserved -or !$parkedProof.activationReuseObserved) { throw 'Parked refill path was not exercised through both deferral and successful activation' }
    }
    $raw | ConvertTo-Json | Set-Content (Join-Path $out 'route-frame-ms.json')
    $attemptRows = @(Select-String -LiteralPath $log -Pattern 'PAA prepare attempts: generation=')
    $attemptMarkers = @(Select-String -LiteralPath $log -Pattern 'PAA prepare attempts: log truncated after 64 conversion rows')
    if ($attemptRows.Count -gt 64 -or $attemptMarkers.Count -gt 1) { throw 'PAA attempt diagnostic log bounds were exceeded' }
    if ($PaaPreparationDiagnostics -and $attemptRows.Count -lt 1) { throw 'Optional packed PAA preparation diagnostic was not exercised' }
    if (!$PaaPreparationDiagnostics -and ($attemptRows.Count -ne 0 -or $attemptMarkers.Count -ne 0)) { throw 'Default run unexpectedly tracked PAA preparation attempts' }
    $attemptProof = @{enabled = [bool]$PaaPreparationDiagnostics; conversionRows = $attemptRows.Count; truncated = $attemptMarkers.Count -eq 1
        rows = @($attemptRows | ForEach-Object { $_.Line })
        scope = 'First at most64 texture-bearing conversions only; filename/generation whole-attempt overlap is not physical identity; continuing diagnostic work invalidates normal throughput acceptance'}
    $attemptProof | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'paa-attempt-proof.json')
    $inflightRows = @(Select-String -LiteralPath $log -Pattern 'PAA prep inflight: actual exact-member suppression observed; suppressed=([1-9]\d*) active=(\d+) metadataBytes=(\d+) peakMetadataBytes=(\d+)')
    if ($PaaPreparationInflight -and $inflightRows.Count -ne 1) { throw 'Exact-member in-flight experiment did not exercise its one bounded suppression proof' }
    if (!$PaaPreparationInflight -and $inflightRows.Count -ne 0) { throw 'Default run unexpectedly suppressed in-flight PAA preparation' }
    if ($inflightRows.Count) {
        $m = $inflightRows[0].Matches[0]
        if ([uint64]$m.Groups[2].Value -gt 128 -or [uint64]$m.Groups[3].Value -gt 1048576 -or [uint64]$m.Groups[4].Value -gt 1048576) {
            throw 'Observed in-flight bookkeeping exceeded its slot or logical retained metadata cap'
        }
    }
    $inflightProof = @{enabled = [bool]$PaaPreparationInflight; suppressionObserved = $inflightRows.Count -eq 1
        rows = @($inflightRows | ForEach-Object { $_.Line })
        scope = 'First exact immutable-member overlapping attempt suppressed; ordinary owner fallback remains. Snapshot is not route totals, pending peer publication success, physical memory or performance acceptance'}
    $inflightProof | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'paa-inflight-proof.json')
    $quotaRows = @(Select-String -LiteralPath $log -Pattern 'Object stream registration quota: reusable=(\d+) needed=(\d+) unknown=(\d+) warmCharged=(\d+) promotions=(\d+) expensiveAttempts=(\d+) limit=(\d+) ownerStateOnly=1')
    if (!$RegistrationQuota -and $quotaRows.Count) { throw 'Default run unexpectedly applied registration-cost quota' }
    $warmChargedObserved = $false
    foreach ($row in $quotaRows) {
        $m = $row.Matches[0]
        if ([uint64]$m.Groups[6].Value -gt [uint64]$m.Groups[7].Value) { throw 'Registration quota exceeded its counted expensive-attempt limit' }
        if ([uint64]$m.Groups[4].Value -ne ([uint64]$m.Groups[2].Value + [uint64]$m.Groups[3].Value) -or
            [uint64]$m.Groups[4].Value -gt [uint64]$m.Groups[6].Value) { throw 'Registration quota classifications and charged work disagree' }
        if ([uint64]$m.Groups[4].Value -gt 0 -and [uint64]$m.Groups[7].Value -eq 2) { $warmChargedObserved = $true }
    }
    if ($RegistrationQuota -and (!$quotaRows.Count -or !$warmChargedObserved)) { throw 'Registration quota did not exercise counted warm registration work' }
    $quotaProof = @{enabled = [bool]$RegistrationQuota; warmChargedObserved = $warmChargedObserved
        rows = @($quotaRows | ForEach-Object { $_.Line })
        scope = 'Sampled existing residency log cadence; counted warm registration scheduling only, not texture readiness, total route work, a frame deadline or cross-machine determinism'}
    $quotaProof | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'registration-quota-proof.json')
    $modelRowEnabled=@(Select-String -LiteralPath $log -SimpleMatch 'gpu MODEL row upload enabled:')
    $modelRowCommits=@(Select-String -LiteralPath $log -Pattern 'gpu MODEL row upload committed: first=(\d+) rows=(\d+) tableRows=(\d+) bytes=(\d+) event=(\d+); postQueueWrite=true sourceRangeOnly=true')
    $modelRowTruncated=@(Select-String -LiteralPath $log -SimpleMatch 'gpu MODEL row upload log truncated:')
    if ($ModelRowUpload -and ($modelRowEnabled.Count -ne 1 -or $modelRowCommits.Count -lt 1 -or
        $modelRowCommits.Count -gt 16 -or $modelRowTruncated.Count -gt 1)) {throw 'MODEL row upload activation/actual partial write proof missing or unbounded'}
    if (!$ModelRowUpload -and ($modelRowEnabled.Count -or $modelRowCommits.Count -or $modelRowTruncated.Count)) {throw 'MODEL row upload activated in OFF control'}
    foreach ($row in $modelRowCommits) {
        $m=$row.Matches[0];$first=[uint64]$m.Groups[1].Value;$rows=[uint64]$m.Groups[2].Value
        $total=[uint64]$m.Groups[3].Value;$bytes=[uint64]$m.Groups[4].Value;$event=[uint64]$m.Groups[5].Value
        if (!$rows -or $rows -ge $total -or $first -gt $total -or $rows -gt ($total-$first) -or
            $bytes -ne ($rows*16) -or !$event -or $event -gt 16) {throw 'Invalid MODEL partial source-range write evidence'}
    }
    $modelRowProof=@{requested=[bool]$ModelRowUpload;activationRows=$modelRowEnabled.Count;
        committedRows=$modelRowCommits.Count;truncatedRows=$modelRowTruncated.Count;
        rows=@($modelRowCommits | ForEach-Object {$_.Line});
        scope='Bounded post-queue-write partial MODEL source ranges; not driver completion, physical memory or performance acceptance'}
    @{passed = [bool]($warmJobsProven -and $warmStageFamilyProven -and $coldAlphaProven); mode = $Mode; admissionMode = $AdmissionMode; parkedTextureRefill = [bool]$ParkedTextureRefill; lazyTextureGroups = [bool]$LazyTextureGroups; warmProfile = [bool]$WarmProfile; screeningOnly = $true; performanceAccepted = $false; terrainPageTimings = [bool]$TerrainPageTimings
        frameCount = $count; dropped = $dropped; enginePhaseTiming = $phases; baseline = $baseline; returned = $returned
        routeSamples = $routeSamples; postExitHashes = $finalHashes
        renderCallCpuTimings = [bool]$TerrainPageTimings; renderCallCpuTimingProof = $callCpuProof
        parkedRefillProof = $parkedProof
        paaLzoReplay = [bool]$PaaLzoReplay; paaLzoReplayProof = $replayProof
        paaPreparationDiagnostics = [bool]$PaaPreparationDiagnostics; paaPreparationProof = $attemptProof
        packedTextures = [bool]$PackedTextures; paaPreparationInflight = [bool]$PaaPreparationInflight
        paaPreparationInflightProof = $inflightProof
        registrationQuota = [bool]$RegistrationQuota; registrationQuotaProof = $quotaProof
        archiveSourceBindings = [bool]$ArchiveSourceBindings; archiveSourceBindingProof = $archiveBindings
        warmTextureJobs = [bool]$WarmTextureJobs; warmTextureJobProof = $warmJobProof
        requireWarmMaterialStageUpload = [bool]$RequireWarmMaterialStageUpload; warmMaterialStageFamilyProof = $warmStageFamilyProof
        coldPaaHandoff = [bool]$ColdPaaHandoff; requireColdAlphaPeek = [bool]$RequireColdAlphaPeek; coldPaaHandoffProof = $coldPaaProof
        modelRowUpload = [bool]$ModelRowUpload; modelRowUploadProof = $modelRowProof
        legacySectionRefresh = [bool]$LegacySectionRefresh; cullSectionRefreshEnvironment = [string]$environment.WGR_CULL_SECTION_REFRESH
        limitation = 'No automatic performance threshold, vanilla parity, cold-cache guarantee or whole-world query readiness; compare matched separate mode processes'
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
    Write-Output "DayZ latency screening evidence: $out"
    if (!$coldAlphaProven) { throw 'NotExercised: no positive real DayZ route alpha peek delta and source-validated cold owned upload; evidence retained' }
    if (!$warmJobsProven) { throw 'Warm preparation did not demonstrate an actual source-validated GPU upload; evidence retained' }
    if (!$warmStageFamilyProven) { throw 'Required bound worker normal/specular filename-family upload not exercised; evidence retained, no material-slot proof claimed' }
} finally {
    if ($client) { $client.Dispose() }
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force }
    foreach ($name in $changedNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
