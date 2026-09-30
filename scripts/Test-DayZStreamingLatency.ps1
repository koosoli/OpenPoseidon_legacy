param(
    [ValidateSet('Disabled', 'Cold')][string]$Mode = 'Disabled',
    [switch]$TerrainPageTimings,
    [switch]$FramePaceTrace,
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
    [switch]$WarmMaterialPreflight,
    [switch]$RapStageEarlyPrepare,
    [switch]$RapBuildingPilot,
    [switch]$RapBuildingMultistage,
    [switch]$DayZPhysicalPrefetch,
    [switch]$DayZProxyPrefetch,
    [switch]$DayZTextureTrace,
    [switch]$DayZDependencyCensus,
    [switch]$PreparedSectionClassification,
    [switch]$TextureBirthObserver,
    [switch]$DayZShapeSplit,
    [switch]$DayZOwnerPrimaryStage,
    [switch]$DayZOwnerNormalStage,
    [switch]$DayZOwnerMaterialStage,
    [switch]$HotSourceProofRetirement,
    [switch]$WeakCacheProof,
    [switch]$WarmTextureProvenance,
    [switch]$WarmEarlySlice,
    [switch]$WarmPublishedReuse,
    [switch]$RequireHotSourceProofRetirement,
    [switch]$RequireWarmMaterialPreflightUpload,
    [switch]$RequireWarmMaterialStageUpload,
    [switch]$LegacySectionRefresh,
    [switch]$SectionReuse,
    [switch]$LodReuse,
    [switch]$ModelRowUpload,
    [switch]$GeometryCopyDiagnostics,
    [switch]$ColdPaaHandoff,
    [switch]$RequireColdAlphaPeek,
    [switch]$RequireColdStartupAlphaPeek,
    [string]$Label = 'dayz-streaming-latency',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if ($RequireColdAlphaPeek -and $RequireColdStartupAlphaPeek) { throw 'Choose exactly one cold alpha validation interval: route or startup admission' }
if ($RequireColdAlphaPeek -or $RequireColdStartupAlphaPeek) { $ColdPaaHandoff = $true }
if ($ColdPaaHandoff) { $ArchiveSourceBindings = $true }
$boundPackedSource = [bool]($PackedTextures -and $ColdPaaHandoff)
if ($RequireWarmMaterialStageUpload) { $WarmTextureJobs = $true; $WarmProfile = $true }
if ($RequireWarmMaterialPreflightUpload) { $WarmMaterialPreflight = $true }
if ($RapStageEarlyPrepare) { $WarmMaterialPreflight = $true; $WarmTextureProvenance = $true }
if ($RapBuildingMultistage) { $RapBuildingPilot = $true }
if ($RapBuildingPilot -or $RapBuildingMultistage) { $WarmMaterialPreflight = $true; $WarmTextureProvenance = $true }
if ($DayZProxyPrefetch) { $DayZPhysicalPrefetch = $true }
if ($TextureBirthObserver) { $HotSourceProofRetirement = $true }
if ($DayZPhysicalPrefetch) { $DayZTextureTrace = $true }
if ($WarmMaterialPreflight) { $WarmTextureJobs = $true; $WarmProfile = $true }
if ($WeakCacheProof) { $HotSourceProofRetirement = $true }
if ($WarmEarlySlice -or $WarmPublishedReuse) { $WarmTextureProvenance = $true }
if ($WarmTextureProvenance) { $WarmTextureJobs = $true; $WarmProfile = $true }
if ($RequireHotSourceProofRetirement) { $HotSourceProofRetirement = $true }
if ($HotSourceProofRetirement) { $WarmTextureJobs = $true; $WarmProfile = $true }
if ($DayZOwnerMaterialStage) { $DayZOwnerPrimaryStage=$true; $WarmTextureJobs=$true; $DayZTextureTrace=$true }
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
        if ($FramePaceTrace) { $argv += '-FramePaceTrace' }
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
        if ($WarmMaterialPreflight) { $argv += '-WarmMaterialPreflight' }
        if ($RapStageEarlyPrepare) { $argv += '-RapStageEarlyPrepare' }
        if ($RapBuildingPilot) { $argv += '-RapBuildingPilot' }
        if ($RapBuildingMultistage) { $argv += '-RapBuildingMultistage' }
        if ($DayZPhysicalPrefetch) { $argv += '-DayZPhysicalPrefetch' }
        if ($DayZProxyPrefetch) { $argv += '-DayZProxyPrefetch' }
        if ($DayZTextureTrace) { $argv += '-DayZTextureTrace' }
        if ($DayZDependencyCensus) { $argv += '-DayZDependencyCensus' }
        if ($PreparedSectionClassification) { $argv += '-PreparedSectionClassification' }
        if ($TextureBirthObserver) { $argv += '-TextureBirthObserver' }
        if ($DayZShapeSplit) { $argv += '-DayZShapeSplit' }
        if ($DayZOwnerPrimaryStage) { $argv += '-DayZOwnerPrimaryStage' }
        if ($DayZOwnerNormalStage) { $argv += '-DayZOwnerNormalStage' }
        if ($DayZOwnerMaterialStage) { $argv += '-DayZOwnerMaterialStage' }
        if ($HotSourceProofRetirement) { $argv += '-HotSourceProofRetirement' }
        if ($WeakCacheProof) { $argv += '-WeakCacheProof' }
        if ($WarmTextureProvenance) { $argv += '-WarmTextureProvenance' }
        if ($WarmEarlySlice) { $argv += '-WarmEarlySlice' }
        if ($WarmPublishedReuse) { $argv += '-WarmPublishedReuse' }
        if ($RequireHotSourceProofRetirement) { $argv += '-RequireHotSourceProofRetirement' }
        if ($RequireWarmMaterialPreflightUpload) { $argv += '-RequireWarmMaterialPreflightUpload' }
        if ($RequireWarmMaterialStageUpload) { $argv += '-RequireWarmMaterialStageUpload' }
        if ($LegacySectionRefresh) { $argv += '-LegacySectionRefresh' }
        if ($SectionReuse) { $argv += '-SectionReuse' }
        if ($LodReuse) { $argv += '-LodReuse' }
        if ($ModelRowUpload) { $argv += '-ModelRowUpload' }
        if ($GeometryCopyDiagnostics) { $argv += '-GeometryCopyDiagnostics' }
        if ($ColdPaaHandoff) { $argv += '-ColdPaaHandoff' }
        if ($RequireColdAlphaPeek) { $argv += '-RequireColdAlphaPeek' }
        if ($RequireColdStartupAlphaPeek) { $argv += '-RequireColdStartupAlphaPeek' }
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
    WGR_OBJECT_STREAM_WARM_MATERIAL_PREFLIGHT = $(if ($WarmMaterialPreflight) { '1' } else { '0' })
    WGR_OBJECT_STREAM_RAP_STAGE_SOURCE = $(if ($RapStageEarlyPrepare) { '1' } else { '0' })
    WGR_OBJECT_STREAM_RAP_STAGE_WARM_CAPTURE = $(if ($RapStageEarlyPrepare -or $RapBuildingPilot -or $RapBuildingMultistage) { '1' } else { '0' })
    WGR_OBJECT_STREAM_RAP_STAGE_EARLY_PREPARE = $(if ($RapStageEarlyPrepare) { '1' } else { '0' })
    WGR_OBJECT_STREAM_RAP_BUILDING_PILOT = $(if ($RapBuildingPilot) { '1' } else { '0' })
    WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE = $(if ($RapBuildingMultistage) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_PHYSICAL_PREFETCH = $(if ($DayZPhysicalPrefetch) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_PROXY_PREFETCH = $(if ($DayZProxyPrefetch) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_TEXTURE_TRACE = $(if ($DayZTextureTrace) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_DEPENDENCY_CENSUS = $(if ($DayZDependencyCensus) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_PREPARED_SECTION = $(if ($PreparedSectionClassification) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER = $(if ($TextureBirthObserver) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_SHAPE_SPLIT = $(if ($DayZShapeSplit) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_OWNER_PRIMARY_STAGE = $(if ($DayZOwnerPrimaryStage) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_OWNER_NORMAL_STAGE = $(if ($DayZOwnerNormalStage) { '1' } else { '0' })
    WGR_OBJECT_STREAM_DAYZ_OWNER_MATERIAL_STAGE = $(if ($DayZOwnerMaterialStage) { '1' } else { '0' })
    WGR_OBJECT_STREAM_WEAK_CACHE_PROOF = $(if ($WeakCacheProof) { '1' } else { '0' })
    WGR_OBJECT_STREAM_WARM_TEXTURE_TRACE = $(if ($WarmTextureProvenance) { '1' } else { '0' })
    WGR_OBJECT_STREAM_WARM_EARLY_SLICE = $(if ($WarmEarlySlice) { '1' } else { '0' })
    WGR_OBJECT_STREAM_WARM_PUBLISHED_REUSE = $(if ($WarmPublishedReuse) { '1' } else { '0' })
    WGR_OBJECT_STREAM_HOT_SOURCE_PROOF_RETIRE = $(if ($HotSourceProofRetirement) { '1' } else { '0' })
}
if ($FramePaceTrace) {
    $environment.POSEIDON_FRAME_TRACE = Join-Path $out 'frame-pace'
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
$screeningFailure = $null; $forcedTermination = $false; $ownedExitCode = $null; $cleanupError = $null
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
        packedTextures = [bool]$PackedTextures; boundPackedSource = $boundPackedSource; paaPreparationInflight = [bool]$PaaPreparationInflight
        registrationQuota = [bool]$RegistrationQuota
        archiveSourceBindings = [bool]$ArchiveSourceBindings
        coldPaaHandoff = [bool]$ColdPaaHandoff; requireColdAlphaPeek = [bool]$RequireColdAlphaPeek
        requireColdStartupAlphaPeek = [bool]$RequireColdStartupAlphaPeek
        coldPaaHandoffEnvironment = [string]$environment.WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF
        warmTextureJobs = [bool]$WarmTextureJobs
        warmMaterialPreflight = [bool]$WarmMaterialPreflight
        rapStageEarlyPrepare = [bool]$RapStageEarlyPrepare
        rapBuildingPilot = [bool]$RapBuildingPilot
        rapBuildingMultistage = [bool]$RapBuildingMultistage
        dayZPhysicalPrefetch = [bool]$DayZPhysicalPrefetch
        dayZProxyPrefetch = [bool]$DayZProxyPrefetch
        dayZTextureTrace = [bool]$DayZTextureTrace
        dayZShapeSplit = [bool]$DayZShapeSplit
        dayZOwnerPrimaryStage = [bool]$DayZOwnerPrimaryStage
        dayZOwnerNormalStage = [bool]$DayZOwnerNormalStage
        dayZOwnerMaterialStage = [bool]$DayZOwnerMaterialStage
        hotSourceProofRetirement = [bool]$HotSourceProofRetirement
        weakCacheProof = [bool]$WeakCacheProof; warmTextureProvenance = [bool]$WarmTextureProvenance
        warmEarlySlice = [bool]$WarmEarlySlice
        warmPublishedReuse = [bool]$WarmPublishedReuse
        requireHotSourceProofRetirement = [bool]$RequireHotSourceProofRetirement
        requireWarmMaterialPreflightUpload = [bool]$RequireWarmMaterialPreflightUpload
        requireWarmMaterialStageUpload = [bool]$RequireWarmMaterialStageUpload
        renderCallCpuTimings = [bool]$TerrainPageTimings
        framePaceTrace = [bool]$FramePaceTrace
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
                'warmTextureQueueBytes','warmTextureMs')) {
                if ($null -eq $camera.$field -or $camera.$field -ne 0) { throw "Disabled warm preparation did work or allocated queue storage: $field" }
            }
            # These counters belong to the shared PreparedTextureStore. The
            # bound cold-PBO experiment and exact-tenement physical prefetch
            # legitimately use the shared store without warm-job queue admission.
            if (!$boundPackedSource -and !$DayZPhysicalPrefetch) {
                foreach ($field in @('warmPreparedPuts','warmPreparedTakes','warmPreparedSourceRefused',
                    'warmPreparedUploads','warmPreparedUploadBytes','warmPreparedUploadFailed')) {
                    if ($null -eq $camera.$field -or $camera.$field -ne 0) { throw "Disabled source-bound preparation did work: $field" }
                }
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
            # Use an actual existing owner snapshot during initial coverage admission,
            # not an assumed zero or a reset/forced reclassification of texture state.
            if ($RequireColdStartupAlphaPeek -and $phase -eq 'baseline' -and $null -eq $script:coldAdmissionFirstSample) {
                $script:coldAdmissionFirstSample = $sample
            }
            if ([DateTime]::UtcNow -gt $deadline) { throw 'Original pose failed to settle in240seconds' }
        } while ($sample.camera.pending -or ($Mode -eq 'Cold' -and !$sample.simulation.metadataComplete))
        if ($sample.camera.resident -ne 19923 -or $sample.camera.desired -ne 19923 -or $sample.camera.centerX -ne 92 -or $sample.camera.centerZ -ne 40) { throw 'Original19,923-camera-object coverage changed' }
        return $sample
    }
    if ($RequireColdStartupAlphaPeek) { $script:coldAdmissionFirstSample = $null }
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
    $warmQueueProven = $returned.camera.warmTextureJobsSubmitted -ge 1 -and
        $returned.camera.warmTextureMembersPrepared -ge 1
    $warmStoreProven = $returned.camera.warmPreparedPuts -ge 1 -and
        $returned.camera.warmPreparedTakes -ge 1 -and $returned.camera.warmPreparedUploads -ge 1 -and
        $returned.camera.warmPreparedUploadBytes -ge 1
    # The building pilot uses the conversion worker, not the separate warm-job queue.
    # The building pilot publishes through the conversion job, with its own
    # exact physical-member upload witness below. It need not run the warm queue.
    $warmJobsProven = !$WarmTextureJobs -or $RapBuildingPilot -or $RapBuildingMultistage -or ($warmStoreProven -and $warmQueueProven)
    $warmJobProof.positive=[bool]$warmJobsProven
    $warmJobProof.required=[bool]($WarmTextureJobs -and !$TextureBirthObserver -and !$DayZOwnerMaterialStage)
    $warmJobProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'warm-texture-job-proof.json')
    $archiveBindings = Send @{cmd = 'archive_source_bindings'}
    if ($archiveBindings.enabled -ne [bool]$ArchiveSourceBindings -or
        $archiveBindings.liveBindings -gt $archiveBindings.capBindings -or
        $archiveBindings.knownCppBytes -gt $archiveBindings.capBytes) { throw 'Actual-read source binding state or cap mismatch' }
    foreach($name in @('cacheHandoffEnabled','weakCells','weakKnownCppBytes','capWeakCells','weakCapacityRefused','weakAllocationRefused','weakWrappersPublished','weakInitializedHandoffs','weakFailedInitCompletions','weakPeakReservationCells','weakPeakReservationBytes')) {
        if($null -eq $archiveBindings.PSObject.Properties[$name]){throw "Missing weak-cache proof state $name"}
    }
    if($archiveBindings.cacheHandoffEnabled -isnot [bool]){throw 'Invalid weak-cache enabled flag type'}
    foreach($name in @('weakCells','weakKnownCppBytes','capWeakCells','weakCapacityRefused','weakAllocationRefused','weakWrappersPublished','weakInitializedHandoffs','weakFailedInitCompletions','weakPeakReservationCells','weakPeakReservationBytes','liveBindings','knownCppBytes')) {
        $value=$archiveBindings.$name
        if($null -eq $value -or $value -is [bool] -or $value -is [string]){throw "Invalid weak-cache counter type $name"}
        $number=[double]$value
        if([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or $number -gt 9007199254740991 -or [Math]::Floor($number) -ne $number){throw "Invalid weak-cache counter value $name"}
    }
    if($archiveBindings.cacheHandoffEnabled -ne [bool]$WeakCacheProof -or $archiveBindings.capWeakCells -ne 1024 -or
        $archiveBindings.weakCells -gt 1024 -or $archiveBindings.weakKnownCppBytes -gt $archiveBindings.knownCppBytes){throw 'Weak-cache metadata flag/cap/accounting mismatch'}
    if(!$WeakCacheProof -and ($archiveBindings.weakCells -ne 0 -or $archiveBindings.weakKnownCppBytes -ne 0 -or
        $archiveBindings.weakCapacityRefused -ne 0 -or $archiveBindings.weakAllocationRefused -ne 0)){throw 'Weak-cache handoff unexpectedly active in OFF control'}
    if($archiveBindings.weakPeakReservationCells -gt 1024 -or $archiveBindings.weakPeakReservationBytes -gt 2097152 -or
        $archiveBindings.weakCells -gt $archiveBindings.weakPeakReservationCells -or
        $archiveBindings.weakKnownCppBytes -gt $archiveBindings.weakPeakReservationBytes -or
        $archiveBindings.weakInitializedHandoffs -gt $archiveBindings.weakWrappersPublished -or
        $archiveBindings.weakFailedInitCompletions -gt $archiveBindings.weakWrappersPublished - $archiveBindings.weakInitializedHandoffs){throw 'Weak-cache cumulative/reservation accounting mismatch'}
    if(!$WeakCacheProof -and ($archiveBindings.weakWrappersPublished -ne 0 -or $archiveBindings.weakInitializedHandoffs -ne 0 -or
        $archiveBindings.weakFailedInitCompletions -ne 0 -or $archiveBindings.weakPeakReservationCells -ne 0 -or
        $archiveBindings.weakPeakReservationBytes -ne 0)){throw 'OFF path unexpectedly recorded weak-cache work'}
    if($WeakCacheProof -and ($archiveBindings.weakWrappersPublished -lt 1 -or $archiveBindings.weakInitializedHandoffs -lt 1 -or
        $archiveBindings.weakPeakReservationCells -lt 1 -or $archiveBindings.weakPeakReservationBytes -lt 1)){throw 'NotExercised: no actual first initialized weak-cache handoff with bounded charged reservation observed'}
    foreach($name in @('warmTraceEnabled','warmTraceSources','warmTraceRows','warmTraceTruncated','traceKnownCppBytes')) {
        if($null -eq $archiveBindings.PSObject.Properties[$name]){throw "Missing warm provenance state $name"}
    }
    if($archiveBindings.warmTraceEnabled -isnot [bool] -or $archiveBindings.warmTraceTruncated -isnot [bool] -or
        $archiveBindings.warmTraceEnabled -ne [bool]$WarmTextureProvenance){throw 'Warm provenance flag/type mismatch'}
    foreach($name in @('warmTraceSources','warmTraceRows','traceKnownCppBytes')){
        $v=$archiveBindings.$name
        if($null -eq $v -or $v -is [bool] -or $v -is [string] -or [double]::IsNaN([double]$v) -or
            [double]::IsInfinity([double]$v) -or [double]$v -lt 0 -or [Math]::Floor([double]$v) -ne [double]$v){throw "Invalid warm provenance counter $name"}
    }
    if($WarmTextureProvenance){
        if($archiveBindings.warmTraceSources -lt 1 -or $archiveBindings.warmTraceSources -gt 8 -or
            $archiveBindings.warmTraceRows -lt 1 -or $archiveBindings.warmTraceRows -gt 128 -or
            $archiveBindings.traceKnownCppBytes -lt 1 -or $archiveBindings.traceKnownCppBytes -gt $archiveBindings.knownCppBytes){throw 'NotExercised: no bounded charged warm provenance events'}
    }elseif($archiveBindings.warmTraceSources -ne 0 -or $archiveBindings.warmTraceRows -ne 0 -or
        $archiveBindings.traceKnownCppBytes -ne 0 -or $archiveBindings.warmTraceTruncated){throw 'OFF path unexpectedly recorded warm provenance'}
    if ($ArchiveSourceBindings) {
        if ($archiveBindings.capBindings -ne 256 -or $archiveBindings.capBytes -ne 2097152 -or
            $archiveBindings.wrappedReads -lt 1 -or $archiveBindings.initRetains -lt 1 -or
            (!$WeakCacheProof -and $archiveBindings.liveBindings -lt 1)) { throw 'Mapped archive provenance did not reach an initialized texture source within its bounds' }
    } elseif ($archiveBindings.liveBindings -ne 0 -or $archiveBindings.knownCppBytes -ne 0 -or
        $archiveBindings.wrappedReads -ne 0 -or $archiveBindings.initRetains -ne 0 -or
        $archiveBindings.captureRefused -ne 0 -or $archiveBindings.capacityRefused -ne 0 -or
        $archiveBindings.allocationRefused -ne 0) { throw 'Default path unexpectedly captured archive source provenance' }
    $archiveBindings | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'archive-source-binding-proof.json')
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'returned-settled.png')}
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) { throw 'Owned game failed to exit cleanly' }
    if ($FramePaceTrace) {
        foreach ($side in @('main', 'producer', 'worker')) {
            $trace = "$($environment.POSEIDON_FRAME_TRACE).$side.csv"
            if (!(Test-Path -LiteralPath $trace -PathType Leaf) -or
                (Get-Item -LiteralPath $trace).Length -lt 100) {
                throw "Missing or empty frame-pace trace: $trace"
            }
        }
    }
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
    $coldStartupProven = !$RequireColdStartupAlphaPeek
    $coldStartupProof = @{required=[bool]$RequireColdStartupAlphaPeek;positive=$false;observedInitialSnapshot=$false
        scope='Actual first observed initial admission snapshot to settled baseline delta plus separate source-validated owned upload witness. Does not alter the independent route-delta requirement or claim the same texture, pixels, GPU completion or performance acceptance'}
    if ($RequireColdStartupAlphaPeek) {
        if ($null -eq $script:coldAdmissionFirstSample) { throw 'Missing initial admission snapshot' }
        $first=$script:coldAdmissionFirstSample
        $delta=$baseline.camera.coldAlphaPeeks-$first.camera.coldAlphaPeeks
        if ($delta -lt 0) { throw 'Cold alpha startup counter went backwards' }
        $coldStartupProven=$delta -gt 0 -and $coldUploadObservations.Count -gt 0
        $coldStartupProof.observedInitialSnapshot=$true;$coldStartupProof.positive=[bool]$coldStartupProven
        $coldStartupProof.firstSnapshotUtc=$first.utc;$coldStartupProof.baselineUtc=$baseline.utc
        $coldStartupProof.firstObservedPeeks=$first.camera.coldAlphaPeeks;$coldStartupProof.baselinePeeks=$baseline.camera.coldAlphaPeeks
        $coldStartupProof.initialAdmissionPeekDelta=$delta;$coldStartupProof.firstObservedResident=$first.camera.resident
        $coldStartupProof.settledResident=$baseline.camera.resident;$coldStartupProof.uploadWitnessCount=$coldUploadObservations.Count
    }
    $coldStartupProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'cold-owned-alpha-startup-proof.json')
    $warmUploadRows = @(Select-String -LiteralPath $log -Pattern 'Warm prepared upload witness: source=(.*?) bytes=(\d+) sourceMemberBytes=(\d+) sourceValidated=true blockUploadSucceeded=true row=(\d+) limit=64')
    $warmUploadObservations = @($warmUploadRows | ForEach-Object {
        $g = $_.Matches[0].Groups
        [pscustomobject]@{ lineNumber = $_.LineNumber; source = [string]$g[1].Value; bytes = [uint64]$g[2].Value
            sourceMemberBytes = [uint64]$g[3].Value; row = [uint64]$g[4].Value
            # ShaderSchema also maps _sm to SpecularDetail (e.g. DayZ roads).
            # This remains a filename-family hint, never proof of an actual slot consumer.
            stageFamily = [bool]($g[1].Value -match '(?i)_(nohq|sm|smdi|dtsmdi)\.paa$') }
    })
    if (@($warmUploadObservations | Where-Object { $_.bytes -eq 0 -or $_.sourceMemberBytes -eq 0 -or
        $_.sourceMemberBytes -gt 16777216 -or $_.row -lt 1 -or $_.row -gt 64 }).Count -gt 0 -or
        $warmUploadObservations.Count -gt 64) { throw 'Malformed bounded warm-upload witness' }
    if ($warmUploadObservations.Count -gt 0 -and !(($WarmTextureJobs -and $WarmProfile) -or $boundPackedSource)) { throw 'Source-bound upload witness unexpectedly active outside requested flags' }
    $warmStageFamilyObserved = @($warmUploadObservations | Where-Object { $_.stageFamily }).Count -gt 0
    $warmStageFamilyProven = !$RequireWarmMaterialStageUpload -or ($warmJobsProven -and $warmStageFamilyObserved)
    $warmStageFamilyProof = @{ required = [bool]$RequireWarmMaterialStageUpload; positive = [bool]$warmStageFamilyObserved
        observations = $warmUploadObservations
        truncated = [bool](Select-String -LiteralPath $log -Pattern 'Warm prepared upload witness truncated:' -Quiet)
        scope = 'Successful source-validated bound worker upload of normal/specular filename family only; not shader slot role, visual quality, whole-model readiness or performance acceptance' }
    $warmStageFamilyProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'warm-material-stage-family-proof.json')
    $buildingCaptureRows = @(Select-String -LiteralPath $log -Pattern 'RVMAT raP building owner capture: modelIndex=\d+ model=(.*?) stage=(.*?) physicalMembersCaptured=1 multistage=false')
    $buildingCaptureStages = @($buildingCaptureRows | ForEach-Object { $_.Matches[0].Groups[2].Value.ToLowerInvariant() })
    $buildingUploadRows = @(Select-String -LiteralPath $log -Pattern 'RVMAT raP physical upload: source=(.*?) bytes=(\d+) memberBytes=(\d+) initMemberMatched=true currentMountMatched=true layoutMatched=true blockUploadSucceeded=true row=(\d+) limit=7')
    $buildingUploads = @($buildingUploadRows | ForEach-Object {
        $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value;bytes=[uint64]$g[2].Value;memberBytes=[uint64]$g[3].Value;row=[uint64]$g[4].Value}
    } | Where-Object { $_.source.ToLowerInvariant() -in $buildingCaptureStages -and $_.bytes -gt 0 -and $_.memberBytes -gt 0 -and $_.row -le 7 })
    $buildingPilotProven = !$RapBuildingPilot -or $RapBuildingMultistage -or ($buildingCaptureStages.Count -gt 0 -and $buildingUploads.Count -gt 0)
    $buildingPilotProof = @{requested=[bool]($RapBuildingPilot -and !$RapBuildingMultistage); positive=[bool]($buildingUploads.Count -gt 0)
        capturedStages=$buildingCaptureStages; uploads=$buildingUploads
        scope='Same named stage has an owner-captured physical PBO member and a later Init-member/current-mount/layout-validated GPU upload; does not prove every building stage/proxy, pixel parity, or latency improvement'}
    $buildingPilotProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'rap-building-pilot-proof.json')
    $multiDiscoveryRows = @(Select-String -LiteralPath $log -Pattern 'RVMAT raP building discovery: modelIndex=\d+ model=dz\\structures\\residential\\tenements\\tenement_small\.p3d material=(.*?) stage=(.*?) candidates=([1-4]) multistage=true')
    $multiCaptureRows = @(Select-String -LiteralPath $log -Pattern 'RVMAT raP building owner capture: modelIndex=\d+ model=dz\\structures\\residential\\tenements\\tenement_small\.p3d stage=(.*?) physicalMembersCaptured=([1-4]) multistage=true')
    $multiKeys = @('dz\structures\data\plaster\plaster_flats02_nohq.paa',
        'dz\structures\data\concrete\concrete_panels_dirty_nohq.paa',
        'dz\structures\data\concrete\concrete_panels_nohq.paa',
        'dz\structures\data\plaster\plaster_flats03_nohq.paa')
    $multiUploads = @($buildingUploadRows | ForEach-Object {
        $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value;bytes=[uint64]$g[2].Value;row=[uint64]$g[4].Value}
    } | Where-Object { $_.source.ToLowerInvariant() -in $multiKeys -and $_.bytes -gt 0 })
    $multiStageProven = !$RapBuildingMultistage -or ($multiDiscoveryRows.Count -gt 0 -and
        $multiCaptureRows.Count -gt 0 -and $multiUploads.Count -gt 0)
    $multiStageProof = @{requested=[bool]$RapBuildingMultistage;positive=[bool]($multiUploads.Count -gt 0)
        discovery=@($multiDiscoveryRows | ForEach-Object { $_.Line });capture=@($multiCaptureRows | ForEach-Object { $_.Line })
        uploads=$multiUploads;scope='Bounded tenement NormalMap stages with same-run discovery, physical owner capture and validated GPU upload; not full material coverage, pixel parity or performance acceptance'}
    $multiStageProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'rap-building-multistage-proof.json')
    $textureTraceHeaders = @(Select-String -LiteralPath $log -Pattern 'DayZ texture first-touch trace: model=(.*?) uploads=(\d+) prepared=(\d+) succeeded=(\d+) measuredReadMs=([0-9.]+) slowestRows=(\d+) limit=16')
    $textureTraceRows = @(Select-String -LiteralPath $log -Pattern 'DayZ texture first-touch row: model=(.*?) rank=(\d+) source=(.*?) readMs=([0-9.]+) bytes=(\d+) prepared=(true|false) uploaded=(true|false) limit=16')
    $textureTraceProven = !$DayZTextureTrace -or ($textureTraceHeaders.Count -eq 3 -and $textureTraceRows.Count -ge 3 -and $textureTraceRows.Count -le 48)
    $textureTraceProof = @{requested=[bool]$DayZTextureTrace; positive=[bool]$textureTraceProven
        headers=@($textureTraceHeaders | ForEach-Object { $_.Line }); rows=@($textureTraceRows | ForEach-Object { $_.Line })
        scope='Top 16 observed source read costs per selected first-touch model, including proxies; not a material-slot role, GPU time, causal speedup or whole-world inventory'}
    $textureTraceProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'dayz-texture-first-touch-proof.json')
    $physicalParentRows = @(Select-String -LiteralPath $log -Pattern 'DayZ physical prefetch: parent model=dz\\structures\\residential\\tenements\\tenement_small\.p3d capturedRaP=(\d+) visitCount=(\d+) captureIncomplete=(true|false) materialSourceBytes=(\d+) maxRaP=16 maxVisits=256')
    $physicalDiscoveryRows = @(Select-String -LiteralPath $log -Pattern 'DayZ physical prefetch: modelIndex=(\d+) parentMaterials=(\d+) stageCandidates=(\d+) candidateOverflow=(true|false) sourceReady=(true|false)')
    $physicalCaptureRows = @(Select-String -LiteralPath $log -Pattern 'DayZ physical prefetch: modelIndex=(\d+) stageCandidates=(\d+) captured=(\d+) sourceRefused=(\d+) candidateOverflow=(true|false) addedStageSourceBytes=(\d+) addedStageSourceLimit=(\d+) complete=false')
    $physicalSourceRows = @(Select-String -LiteralPath $log -Pattern 'DayZ physical prefetch captured: source=(.*?) memberBytes=(\d+) row=(\d+) limit=64')
    $physicalUploadRows = @(Select-String -LiteralPath $log -Pattern 'DayZ physical prefetch upload: source=(.*?) bytes=(\d+) memberBytes=(\d+) initMemberMatched=true currentMountMatched=true layoutMatched=true blockUploadSucceeded=true row=(\d+) limit=(64|96)')
    $physicalUploadLimit = if ($DayZProxyPrefetch) { 96 } else { 64 }
    $physicalParents = @($physicalParentRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{capturedRaP=[uint64]$g[1].Value;visits=[uint64]$g[2].Value
            captureIncomplete=($g[3].Value -eq 'true');materialSourceBytes=[uint64]$g[4].Value} })
    $physicalDiscoveries = @($physicalDiscoveryRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{modelIndex=[uint64]$g[1].Value;parentMaterials=[uint64]$g[2].Value
            stageCandidates=[uint64]$g[3].Value;candidateOverflow=($g[4].Value -eq 'true')
            sourceReady=($g[5].Value -eq 'true')} })
    $physicalCaptures = @($physicalCaptureRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{modelIndex=[uint64]$g[1].Value;stageCandidates=[uint64]$g[2].Value
            captured=[uint64]$g[3].Value;sourceRefused=[uint64]$g[4].Value
            candidateOverflow=($g[5].Value -eq 'true');addedStageSourceBytes=[uint64]$g[6].Value
            addedStageSourceLimit=[uint64]$g[7].Value} })
    $physicalUploads = @($physicalUploadRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value;bytes=[uint64]$g[2].Value
            memberBytes=[uint64]$g[3].Value;row=[uint64]$g[4].Value;limit=[uint64]$g[5].Value} })
    $physicalSources = @($physicalSourceRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value;memberBytes=[uint64]$g[2].Value
            row=[uint64]$g[3].Value} })
    if (@($physicalParents | Where-Object { $_.capturedRaP -gt 16 -or $_.visits -gt 257 -or
        $_.materialSourceBytes -gt 4194304 }).Count -gt 0 -or
        @($physicalDiscoveries | Where-Object { $_.parentMaterials -gt 16 -or $_.stageCandidates -gt 64 }).Count -gt 0 -or
        @($physicalCaptures | Where-Object { $_.stageCandidates -gt 64 -or $_.captured -gt 64 -or
            $_.addedStageSourceLimit -ne 134217728 -or $_.addedStageSourceBytes -gt 134217728 }).Count -gt 0 -or
        @($physicalUploads | Where-Object { !$_.source -or $_.bytes -eq 0 -or $_.memberBytes -eq 0 -or
            $_.memberBytes -gt 16777216 -or $_.row -lt 1 -or $_.row -gt $physicalUploadLimit -or
            $_.limit -ne $physicalUploadLimit }).Count -gt 0 -or
        @($physicalSources | Where-Object { !$_.source -or $_.memberBytes -eq 0 -or
            $_.memberBytes -gt 16777216 -or $_.row -lt 1 -or $_.row -gt 64 }).Count -gt 0 -or
        $physicalSources.Count -gt 64 -or
        $physicalUploads.Count -gt $physicalUploadLimit) { throw 'Malformed bounded DayZ physical prefetch witness' }
    if (!$DayZPhysicalPrefetch -and ($physicalParentRows.Count -or $physicalDiscoveryRows.Count -or
        $physicalCaptureRows.Count -or $physicalSourceRows.Count -or $physicalUploadRows.Count)) {
        throw 'DayZ physical prefetch unexpectedly active in OFF control'
    }
    $physicalPositive = $false
    foreach ($capture in $physicalCaptures) {
        if ($capture.captured -eq 0 -or $capture.addedStageSourceBytes -eq 0) { continue }
        if (@($physicalDiscoveries | Where-Object { $_.modelIndex -eq $capture.modelIndex -and
            $_.stageCandidates -gt 0 -and $_.sourceReady -and
            $_.stageCandidates -eq $capture.stageCandidates }).Count -gt 0) {
            $physicalPositive = $true; break
        }
    }
    $tenementTrace = @($textureTraceHeaders | Where-Object {
        $_.Matches[0].Groups[1].Value -eq 'dz\structures\residential\tenements\tenement_small.p3d' })
    $tenementReadMs = if ($tenementTrace.Count -gt 0) {
        [double]$tenementTrace[0].Matches[0].Groups[5].Value } else { $null }
    $capturedSourceKeys = @($physicalSources | ForEach-Object { $_.source.Replace('\','/').ToLowerInvariant() })
    $physicalSameSourceUploads = @($physicalUploads | Where-Object {
        $_.source.Replace('\','/').ToLowerInvariant() -in $capturedSourceKeys })
    $physicalPrefetchProven = !$DayZPhysicalPrefetch -or ($physicalParents.Count -gt 0 -and
        @($physicalParents | Where-Object { $_.capturedRaP -gt 0 }).Count -gt 0 -and
        $physicalPositive -and $physicalSameSourceUploads.Count -gt 0 -and $null -ne $tenementReadMs)
    $physicalPrefetchProof = @{requested=[bool]$DayZPhysicalPrefetch;positive=[bool]$physicalPrefetchProven
        parent=$physicalParents;discovery=$physicalDiscoveries;capture=$physicalCaptures
        capturedSources=$physicalSources;uploads=$physicalUploads;sameSourceUploads=$physicalSameSourceUploads
        uploadBytes=[uint64](($physicalUploads | Measure-Object -Property bytes -Sum).Sum)
        uploadTruncated=[bool](Select-String -LiteralPath $log -Pattern "DayZ physical prefetch upload truncated: limit=$physicalUploadLimit" -Quiet)
        tenementFirstTouchMeasuredReadMs=$tenementReadMs
        timingScope='Existing selected tenement first-touch trace under the same run; a synchronous read subset, not total frame or causal speedup'
        scope='Bounded parent-IR authored-stage speculative worker preparation with actual physical Take and validated GPU upload. Parent-only, source-specific and partial; no proxy, CPU-complement, material-role or performance acceptance.' }
    $physicalPrefetchProof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'dayz-physical-prefetch-proof.json')
    $proxyCaptureRows = @(Select-String -LiteralPath $log -Pattern 'DayZ proxy prefetch: parent=dz\\structures\\residential\\tenements\\tenement_small\.p3d capturedP3D=(\d+) visits=(\d+) sourceBytes=(\d+) captureIncomplete=(true|false) maxP3D=16 memberLimit=4194304 sourceLimit=33554432 speculative=true')
    $proxyMaterialRows = @(Select-String -LiteralPath $log -Pattern 'DayZ proxy prefetch: modelIndex=(\d+) proxyMaterialCandidates=(\d+) capturedRaP=(\d+) sourceRefused=(\d+) captureIncomplete=(true|false) phase=proxy-material-owner speculative=true')
    $proxyStageRows = @(Select-String -LiteralPath $log -Pattern 'DayZ proxy prefetch: modelIndex=(\d+) stageCandidates=(\d+) capturedPAA=(\d+) sourceRefused=(\d+) candidateOverflow=(true|false) addedSourceBytes=(\d+) sharedStageSourceBytes=(\d+) sharedStageSourceLimit=134217728 complete=false phase=proxy-PAA-owner speculative=true')
    $proxySourceRows = @(Select-String -LiteralPath $log -Pattern 'DayZ proxy prefetch captured: source=(.*?) memberBytes=(\d+) row=(\d+) limit=32 speculative=true')
    $proxySources = @($proxySourceRows | ForEach-Object { $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value;memberBytes=[uint64]$g[2].Value;row=[uint64]$g[3].Value} })
    $proxyUploadedSources = @($physicalUploads | Where-Object {
        $_.source.Replace('\','/').ToLowerInvariant() -in @($proxySources | ForEach-Object { $_.source.Replace('\','/').ToLowerInvariant() }) })
    if (@($proxyCaptureRows | Where-Object { [uint64]$_.Matches[0].Groups[1].Value -gt 16 -or
        [uint64]$_.Matches[0].Groups[2].Value -gt 257 -or
        [uint64]$_.Matches[0].Groups[3].Value -gt 33554432 }).Count -gt 0 -or
        @($proxyMaterialRows | Where-Object { [uint64]$_.Matches[0].Groups[2].Value -gt 16 -or
            [uint64]$_.Matches[0].Groups[3].Value -gt 16 }).Count -gt 0 -or
        @($proxyStageRows | Where-Object { [uint64]$_.Matches[0].Groups[2].Value -gt 32 -or
            [uint64]$_.Matches[0].Groups[3].Value -gt 32 -or
            [uint64]$_.Matches[0].Groups[7].Value -gt 134217728 }).Count -gt 0 -or
        @($proxySources | Where-Object { !$_.source -or $_.memberBytes -eq 0 -or
            $_.memberBytes -gt 16777216 -or $_.row -lt 1 -or $_.row -gt 32 }).Count -gt 0 -or
        $proxySources.Count -gt 32) { throw 'Malformed bounded DayZ proxy prefetch witness' }
    if (!$DayZProxyPrefetch -and ($proxyCaptureRows.Count -or $proxyMaterialRows.Count -or
        $proxyStageRows.Count -or $proxySourceRows.Count)) { throw 'DayZ proxy prefetch unexpectedly active in OFF control' }
    $proxyPrefetchProven = !$DayZProxyPrefetch -or ($proxyCaptureRows.Count -eq 1 -and
        [uint64]$proxyCaptureRows[0].Matches[0].Groups[1].Value -gt 0 -and
        $proxyMaterialRows.Count -gt 0 -and $proxyStageRows.Count -gt 0 -and
        $proxySources.Count -gt 0 -and $proxyUploadedSources.Count -gt 0)
    $proxyPrefetchProof = @{requested=[bool]$DayZProxyPrefetch; positive=[bool]$proxyPrefetchProven
        proxyCapture=@($proxyCaptureRows | ForEach-Object { $_.Line }); materialCapture=@($proxyMaterialRows | ForEach-Object { $_.Line })
        stageCapture=@($proxyStageRows | ForEach-Object { $_.Line }); capturedSources=$proxySources
        sameSourceUploads=$proxyUploadedSources; uploadBytes=[uint64](($proxyUploadedSources | Measure-Object -Property bytes -Sum).Sum)
        uploadWitnessTruncated=[bool](Select-String -LiteralPath $log -Pattern "DayZ physical prefetch upload truncated: limit=$physicalUploadLimit" -Quiet)
        partial=$true; scope='Bounded speculative one-hop proxy P3D source-byte material candidates and raP preparation. Same-source physical upload proves only a prepared PAA was actually consumed; the lexical scan is not IR-complete and config selection, role, proxy coverage and performance remain unproven.' }
    $proxyPrefetchProof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'dayz-proxy-prefetch-proof.json')
    $shapeSplitRows = @(Select-String -LiteralPath $log -Pattern 'DayZ shape install split: model=dz\\structures\\residential\\tenements\\tenement_small\.p3d worldCalled=true worldRegisterMs=([0-9.]+) legacyOptimizeMs=([0-9.]+) totalOptimizeMs=([0-9.]+)')
    $shapeSplitProven = !$DayZShapeSplit -or $shapeSplitRows.Count -eq 1
    $shapeSplitProof = @{requested=[bool]$DayZShapeSplit; positive=[bool]$shapeSplitProven
        rows=@($shapeSplitRows | ForEach-Object { $_.Line })
        scope='One selected cold DayZ shape; separates WorldShapeLoaded from legacy OptimizeOneShape without changing either call. Neither split proves a removable cost or matched performance.'}
    $shapeSplitProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'dayz-shape-install-split-proof.json')
    $ownerStageStarts = @(Select-String -LiteralPath $log -Pattern 'DayZ owner primary stage: started modelIndex=\d+ textures=\d+ reservedBytes=\d+ ordinarySource=true')
    $ownerStageSteps = @(Select-String -LiteralPath $log -Pattern 'DayZ owner primary stage: step=\d+ total=\d+ modelIndex=\d+ texture=.* uploaded=true stepMs=[0-9.]+ maxStepMs=[0-9.]+ row=\d+ limit=64')
    $ownerStageHandoffs = @(Select-String -LiteralPath $log -Pattern 'DayZ owner primary stage: handoff modelIndex=\d+ originPlacement=\d+ consumingPlacement=\d+ admitted=true uploadSteps=\d+ lostBeforeRegister=\d+ uploadFailure=false stepMs=[0-9.]+ maxStepMs=[0-9.]+ waitToCreateMs=[0-9.]+ finalCreateMs=[0-9.]+ row=\d+ limit=8')
    $ownerStageProven = !$DayZOwnerPrimaryStage -or ($ownerStageStarts.Count -eq 1 -and
        $ownerStageSteps.Count -gt 0 -and $ownerStageSteps.Count -le 64 -and $ownerStageHandoffs.Count -eq 1)
    $ownerStageProof = @{requested=[bool]$DayZOwnerPrimaryStage; positive=[bool]$ownerStageProven
        starts=@($ownerStageStarts | ForEach-Object { $_.Line }); steps=@($ownerStageSteps | ForEach-Object { $_.Line });
        handoffs=@($ownerStageHandoffs | ForEach-Object { $_.Line })
        scope='One exact DayZ tenement staged through ordinary owner primary-texture uploads before unchanged complete model admission; not RVMAT/proxy coverage, a per-upload deadline, visual or performance acceptance.'}
    $ownerStageProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'dayz-owner-primary-stage-proof.json')
    $normalStageUploads = @(Select-String -LiteralPath $log -Pattern 'DayZ owner normal stage: role=NormalMap material=.* texture=.* bytes=\d+ lod=\d+ section=\d+ row=\d+ limit=16')
    $normalStageHandoffs = @(Select-String -LiteralPath $log -Pattern 'DayZ owner normal stage: handoff modelIndex=\d+ done=(true|false) fallback=(true|false) reason=.* sections=\d+ attempts=\d+ uploads=\d+ stagedDistinct=\d+ captured=[1-9]\d* bytes=\d+ stepMs=[0-9.]+ maxStepMs=[0-9.]+ admitted=true')
    $normalStageProven = !$DayZOwnerNormalStage -or ($DayZOwnerPrimaryStage -and
        $normalStageUploads.Count -gt 0 -and $normalStageHandoffs.Count -eq 1)
    $normalStageProof = @{requested=[bool]$DayZOwnerNormalStage; positive=[bool]$normalStageProven
        uploads=@($normalStageUploads | ForEach-Object { $_.Line }); handoffs=@($normalStageHandoffs | ForEach-Object { $_.Line })
        scope='Exact parent NormalMap staging with a completed-model captured-binding witness; no proxy or performance acceptance.'}
    $normalStageProof | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'dayz-owner-normal-stage-proof.json')
    $materialStageRows=@(Select-String -LiteralPath $log -Pattern 'DayZ owner material stage: modelIndex=\d+ lod=\d+ section=\d+ role=.* attempted=(true|false) uploaded=(true|false) failed=(true|false) done=(true|false) roles=\d+ uploads=\d+ bytes=\d+ stepMs=[0-9.]+ maxStepMs=[0-9.]+ row=\d+ limit=128 scope=parent-declarations-partial')
    $materialStageHandoffs=@(Select-String -LiteralPath $log -Pattern 'DayZ owner material stage: handoff modelIndex=\d+ done=(true|false) fallback=(true|false) reason=.* sections=\d+ roleAttempts=\d+ uploads=\d+ stagedDistinct=\d+ bytes=\d+ stepMs=[0-9.]+ maxStepMs=[0-9.]+ admitted=true scope=parent-declarations-partial-final-registration-ordinary')
    $materialStageFinal=@(Select-String -LiteralPath $log -Pattern 'DayZ owner material stage: final-registration remainingUploads=\d+ stagedParentUploads=\d+ scope=includes-proxies-and-other-uncovered-roles-not-completeness')
    $materialStageProven= !$DayZOwnerMaterialStage -or ($materialStageRows.Count -gt 0 -and
        $materialStageRows.Count -le 128 -and @($materialStageRows | Where-Object {$_.Line -match ' uploaded=true '}).Count -gt 0 -and
        $materialStageHandoffs.Count -eq 1 -and $materialStageFinal.Count -eq 1)
    if(!$DayZOwnerMaterialStage -and ($materialStageRows.Count -or $materialStageHandoffs.Count -or $materialStageFinal.Count)){throw 'Parent material stage unexpectedly active in OFF control'}
    $materialStageProof=@{requested=[bool]$DayZOwnerMaterialStage;positive=[bool]$materialStageProven;
        rows=@($materialStageRows|ForEach-Object {$_.Line});handoffs=@($materialStageHandoffs|ForEach-Object {$_.Line});
        final=@($materialStageFinal|ForEach-Object {$_.Line});
        scope='Partial current-source parent material-role staging only; final registration remains ordinary and may upload proxy/uncovered dependencies; no complete readiness or performance acceptance'}
    $materialStageProof|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'dayz-owner-material-stage-proof.json')
    # Separate warm-preflight proof still uses its own bound-upload witness.
    # The physical building pilot above uses its explicit Init/mount/layout witness.
    $preflightRawRows = @(Select-String -LiteralPath $log -Pattern 'Warm material source preflight: source=')
    $preflightRows = @(Select-String -LiteralPath $log -Pattern 'Warm material source preflight: source=(.*?) attemptedLoad=true textureReturned=(true|false) threw=(true|false) textureUsable=(true|false) currentSource=(true|false) rejection=(NotInspected|VisitCap|NoWgpuTexture|SourceNotInitialized|NoUsableMips|AlreadyGpuResident|UploadAlreadyTried|NonReloadable|NoInitializedBinding|NoMountedMatch|Oversize|InvalidKey|DuplicateKey|Accepted|Exception) statsComparable=(true|false) globalCaptureRefusedDelta=(\d+) globalCapacityRefusedDelta=(\d+) globalAllocationRefusedDelta=(\d+) boundReadAccepted=(true|false) sourceMs=([0-9.]+) remainingAttempts=(\d+) metadataWorkLeft=(\d+) row=(\d+) limit=64')
    $preflightObservations = @($preflightRows | ForEach-Object {
        $g = $_.Matches[0].Groups
        [pscustomobject]@{source=[string]$g[1].Value; returned=$g[2].Value -eq 'true'; threw=$g[3].Value -eq 'true'
            textureUsable=$g[4].Value -eq 'true'; currentSource=$g[5].Value -eq 'true'; rejection=[string]$g[6].Value
            statsComparable=$g[7].Value -eq 'true'; globalCaptureRefusedDelta=[uint64]$g[8].Value
            globalCapacityRefusedDelta=[uint64]$g[9].Value; globalAllocationRefusedDelta=[uint64]$g[10].Value
            accepted=$g[11].Value -eq 'true'; sourceMs=[double]::Parse($g[12].Value,[Globalization.CultureInfo]::InvariantCulture)
            remainingAttempts=[uint64]$g[13].Value; metadataWorkLeft=[uint64]$g[14].Value; row=[uint64]$g[15].Value}
    })
    if (@($preflightObservations | Where-Object { !$_.source -or $_.remainingAttempts -gt 1 -or $_.metadataWorkLeft -gt 256 -or
        $_.row -lt 1 -or $_.row -gt 64 -or ([double]::IsNaN($_.sourceMs) -or [double]::IsInfinity($_.sourceMs)) -or $_.sourceMs -lt 0 -or
        ($_.accepted -and (!$_.returned -or $_.threw -or !$_.textureUsable -or !$_.currentSource -or $_.rejection -ne 'Accepted')) -or
        ($_.threw -ne ($_.rejection -eq 'Exception')) -or
        ($_.currentSource -and !$_.textureUsable) -or ($_.rejection -eq 'Accepted' -and !$_.accepted) -or
        ($_.rejection -in @('NoInitializedBinding','NoMountedMatch') -and ($_.currentSource -or !$_.textureUsable)) -or
        (!$_.statsComparable -and ($_.globalCaptureRefusedDelta -or $_.globalCapacityRefusedDelta -or $_.globalAllocationRefusedDelta))
    }).Count -gt 0 -or $preflightObservations.Count -gt 64 -or
        $preflightRawRows.Count -ne $preflightObservations.Count) {throw 'Malformed bounded warm-material preflight witness'}
    if (!$WarmMaterialPreflight -and $preflightObservations.Count) {throw 'Warm-material preflight unexpectedly active in OFF control'}
    $warmUploadedStageKeys = @($warmUploadObservations | Where-Object {$_.stageFamily} | ForEach-Object {$_.source.Replace('\','/').ToLowerInvariant()})
    $joinedPreflightUploads = @($preflightObservations | Where-Object {$_.accepted -and $_.source.Replace('\','/').ToLowerInvariant() -in $warmUploadedStageKeys})
    $warmPreflightProven = !$RequireWarmMaterialPreflightUpload -or $joinedPreflightUploads.Count -gt 0
    $warmPreflightProof = @{requested=[bool]$WarmMaterialPreflight; required=[bool]$RequireWarmMaterialPreflightUpload
        positive=[bool]($joinedPreflightUploads.Count -gt 0); observations=$preflightObservations; sameSourceStageUploads=$joinedPreflightUploads
        truncated=[bool](Select-String -LiteralPath $log -Pattern 'Warm material source preflight: log truncated after 64' -Quiet)
        scope='Bounded actual owner missing-stage loads; currentSource is exact currently mounted binding or false for absent/mismatched source. Global counter deltas include concurrent activity and are not per-load causal proof. Same-source upload join does not prove slot role, pixels or performance.'}
    $warmPreflightProof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'warm-material-preflight-proof.json')
    $retirementRows = @(Select-String -LiteralPath $log -Pattern 'Hot source proof retired: source=(.*?) ownInitProofDropped=true alphaClass=(\d+) gpuUploaded=true liveBindingsBefore=(\d+) liveBindingsAfter=(\d+) knownBindingBytesBefore=(\d+) knownBindingBytesAfter=(\d+) aliasDebtMayRemain=true row=(\d+) limit=64')
    $retirementObservations = @($retirementRows | ForEach-Object {
        $g=$_.Matches[0].Groups
        [pscustomobject]@{lineNumber=$_.LineNumber; source=[string]$g[1].Value; alphaClass=[uint64]$g[2].Value
            liveBefore=[uint64]$g[3].Value; liveAfter=[uint64]$g[4].Value
            knownBefore=[uint64]$g[5].Value; knownAfter=[uint64]$g[6].Value; row=[uint64]$g[7].Value}
    })
    if ($retirementObservations.Count -gt 64 -or @($retirementObservations | Where-Object {
        !$_.source -or $_.alphaClass -gt 2 -or $_.liveBefore -lt 1 -or $_.liveBefore -gt 256 -or
        $_.knownBefore -lt 1 -or $_.knownBefore -gt 2097152 -or $_.liveAfter -gt $_.liveBefore -or $_.knownAfter -gt $_.knownBefore -or $_.row -lt 1 -or $_.row -gt 64
    }).Count) {throw 'Malformed bounded hot source proof retirement witness'}
    $retirementTruncated=[bool](Select-String -LiteralPath $log -Pattern 'Hot source proof retired: log truncated after 64 owner drops;' -Quiet)
    if (!$HotSourceProofRetirement -and ($retirementObservations.Count -or $retirementTruncated)) {throw 'Hot source proof retirement unexpectedly active in OFF control'}
    # Preserve the outer upload line while examining prior retirement observations.
    $laterStageUploads=@($warmUploadObservations | Where-Object {
        $uploadLine=$_.lineNumber
        $_.stageFamily -and @($retirementObservations | Where-Object {$_.lineNumber -lt $uploadLine}).Count -gt 0
    })
    $hotRetirementPositive=$retirementObservations.Count -gt 0 -and $laterStageUploads.Count -gt 0
    $hotRetirementProven= !$RequireHotSourceProofRetirement -or $hotRetirementPositive
    $hotRetirementProof=@{requested=[bool]$HotSourceProofRetirement;required=[bool]$RequireHotSourceProofRetirement
        positive=[bool]$hotRetirementPositive;observations=$retirementObservations;laterSourceValidatedStageUploads=$laterStageUploads
        observedLogicalBindingDrops=@($retirementObservations | Where-Object {$_.liveAfter -lt $_.liveBefore}).Count
        truncated=$retirementTruncated
        scope='Actual owner Init-proof drops and later successful source-validated bound worker filename-family uploads; retained aliases may keep logical debt. No causal capture-slot, source-freshness, physical-memory or performance proof'}
    $hotRetirementProof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'hot-source-proof-retirement-proof.json')

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
    $earlyRows = @(Select-String -LiteralPath $log -Pattern 'Warm provenance: row=\d+ token=\d+ event=earlyWorkerStarted a=(\d+) b=(\d+)')
    foreach ($row in $earlyRows) {
        if ([uint64]$row.Matches[0].Groups[1].Value -lt 1 -or
            [uint64]$row.Matches[0].Groups[2].Value -lt 1) { throw 'Invalid actual early warm selection witness' }
    }
    if ($earlyRows.Count -gt 32 -or (!$WarmEarlySlice -and $earlyRows.Count)) { throw 'Unbounded or unexpected early warm selection' }
    $earlyProof = @{requested=[bool]$WarmEarlySlice; witnessed=$earlyRows.Count;
        rows=@($earlyRows | ForEach-Object {$_.Line});
        scope='Actual first-member dequeue with a positive cold queue; not useful upload, FPS or cold-throughput acceptance'}
    $earlyProven = !$WarmEarlySlice -or $earlyRows.Count -gt 0
    $promotionRows = @(Select-String -LiteralPath $log -Pattern 'Warm provenance: row=\d+ token=\d+ event=storePromotedPublished a=0 b=0')
    if ($promotionRows.Count -gt 32 -or (!$WarmPublishedReuse -and $promotionRows.Count)) { throw 'Unbounded or unexpected published warm promotion' }
    $promotionProof = @{requested=[bool]$WarmPublishedReuse; witnessed=$promotionRows.Count;
        rows=@($promotionRows | ForEach-Object {$_.Line});
        scope='Historical exact-member promotion witness; not a later Take, source match, upload, or complete 32-entry scan'}
    $promotionProven = !$WarmPublishedReuse -or $promotionRows.Count -gt 0
    $preparedRows=@(Select-String -LiteralPath $log -Pattern 'Prepared GPU section: route=([0-6]) accepted=(true|false) variant=([01]) alphaRef=([0-9.eE+-]+) texture=(\d+) facts=31 scope=live-owner-classification')
    if($preparedRows.Count -gt 64 -or (!$PreparedSectionClassification -and $preparedRows.Count)){throw 'Prepared classification activated unexpectedly or exceeded trace bound'}
    foreach($row in $preparedRows){
        $m=$row.Matches[0];$route=[int]$m.Groups[1].Value;$accepted=$m.Groups[2].Value -eq 'true';$variant=[int]$m.Groups[3].Value;$alpha=[double]$m.Groups[4].Value
        if([double]::IsNaN($alpha) -or [double]::IsInfinity($alpha) -or $alpha -lt 0 -or $alpha -gt 1 -or ($accepted -ne ($route -in @(5,6))) -or ($accepted -and (($variant -eq 1) -ne ($alpha -gt 0)))){throw 'Prepared classifier returned an inconsistent route/variant/alpha'}
    }
    $preparedAccepted=@($preparedRows|Where-Object {$_.Matches[0].Groups[2].Value -eq 'true'})
    if($PreparedSectionClassification -and !$preparedAccepted.Count){throw 'NotExercised: no actual selected-tenement prepared GPU-owned section'}
    $preparedProof=@{requested=[bool]$PreparedSectionClassification;rows=@($preparedRows|ForEach-Object {$_.Line});acceptedRows=$preparedAccepted.Count;
        scope='First at most64 actual selected-tenement owner fact classifications; face upload remains synchronous, no complete role/model admission, source freshness, pixel parity or performance claim'}
    $birthRows=@(Select-String -LiteralPath $log -Pattern 'DayZ texture birth observer: calls=(\d+) duplicates=(\d+) captured=(\d+) physical=(\d+) generated=(\d+) missingSource=(\d+) unsupportedDynamic=(\d+) invalidName=(\d+) capacityRefused=(\d+) partial=(true|false) currentPhysical=(\d+) resident=(\d+) knownPacketBytes=(\d+) finalRoleClosure=false scope=before-Ensure-upload-owner')
    if (!$TextureBirthObserver -and $birthRows.Count) { throw 'Texture birth observer unexpectedly activated' }
    $birthPositive=$false
    foreach($row in $birthRows) {
        $m=$row.Matches[0];$captured=[int]$m.Groups[3].Value;$physical=[int]$m.Groups[4].Value;$generated=[int]$m.Groups[5].Value
        $current=[int]$m.Groups[11].Value;$resident=[int]$m.Groups[12].Value
        if($captured -gt 128 -or $captured -ne ($physical+$generated) -or $current -gt $physical -or $resident -gt $captured){throw 'Texture birth observer exceeded bounds or returned inconsistent ownership'}
        if($physical -gt 0 -and $current -eq $physical -and $resident -eq $captured){$birthPositive=$true}
    }
    if($TextureBirthObserver -and !$birthPositive){throw 'NotExercised: no captured physical source births remained current and resident through selected registration'}
    $birthProof=@{requested=[bool]$TextureBirthObserver;positive=$birthPositive;rows=@($birthRows|ForEach-Object {$_.Line});scope='Selected owner before-Ensure unique texture refs and Init archive leases; unknown births remain partial, no exhaustive roles or paced registration/performance claim'}
    @{passed = [bool](($warmJobsProven -or $TextureBirthObserver -or $DayZOwnerMaterialStage) -and $warmStageFamilyProven -and $warmPreflightProven -and $hotRetirementProven -and $coldAlphaProven -and $coldStartupProven -and $earlyProven -and $promotionProven -and $buildingPilotProven -and $multiStageProven -and $physicalPrefetchProven -and $proxyPrefetchProven -and $textureTraceProven -and $shapeSplitProven -and $ownerStageProven -and $normalStageProven -and $materialStageProven); mode = $Mode; admissionMode = $AdmissionMode; parkedTextureRefill = [bool]$ParkedTextureRefill; lazyTextureGroups = [bool]$LazyTextureGroups; warmProfile = [bool]$WarmProfile; screeningOnly = $true; performanceAccepted = $false; terrainPageTimings = [bool]$TerrainPageTimings; framePaceTrace = [bool]$FramePaceTrace
        frameCount = $count; dropped = $dropped; enginePhaseTiming = $phases; baseline = $baseline; returned = $returned
        routeSamples = $routeSamples; postExitHashes = $finalHashes
        renderCallCpuTimings = [bool]$TerrainPageTimings; renderCallCpuTimingProof = $callCpuProof
        parkedRefillProof = $parkedProof
        paaLzoReplay = [bool]$PaaLzoReplay; paaLzoReplayProof = $replayProof
        paaPreparationDiagnostics = [bool]$PaaPreparationDiagnostics; paaPreparationProof = $attemptProof
        packedTextures = [bool]$PackedTextures; boundPackedSource = $boundPackedSource; paaPreparationInflight = [bool]$PaaPreparationInflight
        paaPreparationInflightProof = $inflightProof
        registrationQuota = [bool]$RegistrationQuota; registrationQuotaProof = $quotaProof
        archiveSourceBindings = [bool]$ArchiveSourceBindings; archiveSourceBindingProof = $archiveBindings
        weakCacheProof = [bool]$WeakCacheProof; weakCacheProofState = $archiveBindings
        warmTextureProvenance = [bool]$WarmTextureProvenance
        warmEarlySlice = [bool]$WarmEarlySlice; warmEarlySliceProof = $earlyProof
        warmPublishedReuse = [bool]$WarmPublishedReuse; warmPublishedReuseProof = $promotionProof
        hotSourceProofRetirement = [bool]$HotSourceProofRetirement; hotSourceProofRetirementProof = $hotRetirementProof
        warmMaterialPreflight = [bool]$WarmMaterialPreflight; warmMaterialPreflightProof = $warmPreflightProof
        rapStageEarlyPrepare = [bool]$RapStageEarlyPrepare
        rapBuildingPilot = [bool]$RapBuildingPilot; rapBuildingPilotProof = $buildingPilotProof
        rapBuildingMultistage = [bool]$RapBuildingMultistage
        rapBuildingMultistageProof = $multiStageProof
        dayZPhysicalPrefetch = [bool]$DayZPhysicalPrefetch
        dayZPhysicalPrefetchProof = $physicalPrefetchProof
        dayZProxyPrefetch = [bool]$DayZProxyPrefetch
        dayZProxyPrefetchProof = $proxyPrefetchProof
        dayZTextureTrace = [bool]$DayZTextureTrace
        dayZTextureTraceProof = $textureTraceProof
        dayZShapeSplit = [bool]$DayZShapeSplit
        preparedSectionClassification=[bool]$PreparedSectionClassification;preparedSectionProof=$preparedProof
        textureBirthObserver=[bool]$TextureBirthObserver;textureBirthObserverProof=$birthProof
        dayZShapeSplitProof = $shapeSplitProof
        dayZOwnerPrimaryStage = [bool]$DayZOwnerPrimaryStage
        dayZOwnerPrimaryStageProof = $ownerStageProof
        dayZOwnerNormalStage = [bool]$DayZOwnerNormalStage
        dayZOwnerMaterialStage = [bool]$DayZOwnerMaterialStage
        dayZOwnerNormalStageProof = $normalStageProof
        dayZOwnerMaterialStageProof = $materialStageProof
        warmTextureJobs = [bool]$WarmTextureJobs; warmTextureJobProof = $warmJobProof
        requireWarmMaterialStageUpload = [bool]$RequireWarmMaterialStageUpload; warmMaterialStageFamilyProof = $warmStageFamilyProof
        coldPaaHandoff = [bool]$ColdPaaHandoff; requireColdAlphaPeek = [bool]$RequireColdAlphaPeek; coldPaaHandoffProof = $coldPaaProof
        requireColdStartupAlphaPeek = [bool]$RequireColdStartupAlphaPeek; coldPaaStartupProof = $coldStartupProof
        modelRowUpload = [bool]$ModelRowUpload; modelRowUploadProof = $modelRowProof
        legacySectionRefresh = [bool]$LegacySectionRefresh; cullSectionRefreshEnvironment = [string]$environment.WGR_CULL_SECTION_REFRESH
        limitation = 'No automatic performance threshold, vanilla parity, cold-cache guarantee or whole-world query readiness; compare matched separate mode processes'
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
    Write-Output "DayZ latency screening evidence: $out"
    if (!$coldStartupProven) { throw 'NotExercised: no positive observed initial DayZ admission alpha delta and source-validated owned upload; evidence retained' }
    if (!$coldAlphaProven) { throw 'NotExercised: no positive real DayZ route alpha peek delta and source-validated cold owned upload; evidence retained' }
    if (!$hotRetirementProven) { throw 'NotExercised: no actual owner proof drop and later successful source-validated worker stage upload; evidence retained' }
    if (!$earlyProven) { throw 'NotExercised: no actual early warm first-member dequeue; evidence retained' }
    if (!$promotionProven) { throw 'NotExercised: no published warm entry promoted before model cancellation; evidence retained' }
    if (!$warmPreflightProven) { throw 'NotExercised: no same-source initialized-stage preflight and successful bound worker stage upload; evidence retained' }
    # Birth observation needs retirement to leave space in the bounded lease pool.
    # Its positive witness is current physical births, not a new worker upload.
    # Keep the independent warm-upload result visible; explicit Require* checks
    # below still require their respective upload witnesses.
    if (!$warmJobsProven -and !$TextureBirthObserver -and !$DayZOwnerMaterialStage) { throw 'Warm preparation did not demonstrate an actual source-validated GPU upload; evidence retained' }
    if (!$buildingPilotProven) { throw 'NotExercised: no same-stage building physical capture and validated upload; evidence retained' }
    if (!$multiStageProven) { throw 'NotExercised: no same-stage tenement multistage physical capture and validated upload; evidence retained' }
    if (!$physicalPrefetchProven) { throw 'NotExercised: no bounded parent raP stage capture and source-validated physical Take/upload for exact tenement; evidence retained' }
    if (!$proxyPrefetchProven) { throw 'NotExercised: no bounded speculative proxy P3D/raP source capture and same-source physical Take/upload; evidence retained' }
    if (!$textureTraceProven) { throw 'NotExercised: selected DayZ building texture trace incomplete; evidence retained' }
    if (!$shapeSplitProven) { throw 'NotExercised: selected DayZ shape install split absent; evidence retained' }
    if (!$ownerStageProven) { throw 'NotExercised: selected DayZ owner primary stage incomplete; evidence retained' }
    if (!$materialStageProven) { throw 'NotExercised: parent material stage did not produce a current upload and ordinary final-registration handoff; evidence retained' }
    if (!$normalStageProven) { throw 'NotExercised: selected DayZ owner normal stage lacks a captured completed-model NormalMap; evidence retained' }
    if (!$warmStageFamilyProven) { throw 'Required bound worker normal/specular filename-family upload not exercised; evidence retained, no material-slot proof claimed' }
} catch {
    $screeningFailure = $_.Exception.Message
    throw
} finally {
    try {
        if ($p -and !$p.HasExited) {
            # A failed verification still requests normal cleanup from this owned PID.
            try {
                if ($client -and $client.Connected) {
                    $null = Send @{cmd = 'exit'}
                    $null = $p.WaitForExit(15000)
                }
            } catch {}
            if (!$p.HasExited) {
                $forcedTermination = $true
                Stop-Process -Id $p.Id -Force
                $null = $p.WaitForExit(5000)
            }
        }
        if ($p -and $p.HasExited) { $ownedExitCode = $p.ExitCode }
    } catch { $cleanupError = $_.Exception.Message }
    if ($client) { $client.Dispose() }
    foreach ($name in $changedNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    # Save early refusals as well as late result failures; never infer exit0 from a log.
    try {
        $shutdownComplete = (Test-Path -LiteralPath $log) -and
            [bool](Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)
        $cleanup = [ordered]@{
            ownedPid = if ($p) { $p.Id } else { $null }
            ownedExitCode = $ownedExitCode; forcedTermination = $forcedTermination
            shutdownComplete = $shutdownComplete; cleanupError = $cleanupError
            screeningFailure = $screeningFailure
        }
        $cleanup | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'cleanup.json')
        if ($null -ne $screeningFailure) {
            @{ passed = $false; reason = $screeningFailure; cleanup = $cleanup } |
                ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'failure.json')
        }
    } catch { Write-Warning "Could not preserve screening cleanup evidence: $($_.Exception.Message)" }
}
