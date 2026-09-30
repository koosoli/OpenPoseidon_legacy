# Installed stock roof falsifier. Run only through with-game-lock.sh.
[CmdletBinding()]
param(
    [double]$RoofX = 9486, [double]$RoofZ = 3006,
    [double]$ControlX = 9475.25, [double]$ControlZ = 3018.25,
    [string]$Camera = '9481.25 2994.25 209.72 0 -18',
    [ValidateRange(1,15)][int]$SettleSeconds = 3,
    [ValidateRange(1,64)][int]$MaskThreshold = 8,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'stock-roof',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
function Camera-Numbers([string]$value) {
    $tokens = @($value.Trim() -split '\s+')
    if ($tokens.Count -ne 5) { throw 'Camera must contain X Z absolute-height azimuth elevation.' }
    foreach ($token in $tokens) {
        $number = 0.0
        if (![double]::TryParse($token, [Globalization.NumberStyles]::Float, $culture, [ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "Invalid camera number: $token" }
    }
    return $tokens
}
$numbers = @(Camera-Numbers $Camera)
$root = Split-Path -Parent $PSScriptRoot
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in @($mission, $NativeAddons)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required native fixture path missing: $path" }
}
function Installed-State {
    # Provenance is the first installed file read, before hashes or launch.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Installed deployment provenance is empty.' }
    $files = @('OpenPoseidon.exe', 'wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
$before = Installed-State
$output = Join-Path $root ('build/terrain-puddles/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; scope = 'stock roof retained identity and dry/wet absent/present/removed pixel controls';
    before = $before; arms = @{}; comparisons = @{}; gates = @{}; error = $null;
    roof = @{ status = 'not-captured' } }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLES',
    'WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_INTERIOR_SKY','WGR_INTERIOR_SKY_DEBUG',
    'POSEIDON_INTERIOR_SKY_PROBE','WGR_GRASS','POSEIDON_WIND_OVERRIDE','WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
@{ scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash;
   metricsSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs')).Hash;
   sourceHead = (& git -C $root rev-parse HEAD); installed = $before; mission = $mission;
   nativeAddons = $NativeAddons; camera = $Camera; stockRoofClass = 'CampEastC'; probeFrame = 3600; roofCentre = @($RoofX,$RoofZ); controlCentre = @($ControlX,$ControlZ);
   lockOwner = $env:LOCK_OWNER; fixedSimTime = 100; fixedDate = @(1985,6,21,16,0); fixedExposure = 1; roi = 'central 60% of width and height';
   maskThreshold = $MaskThreshold; fixture = 'runtime heights and actual stock CampEastC over verified native dirt';
   wetnessOverrides = @(0,1); actualRain = 0; temporal = 0;
   grass = 0; windOverride = '0 90 0'; cloudCoverage = 0; fixedLodRange = 1; terrainJitter = 0;
   isolation = 'Terrain response controls exclude grass and pin wind/clouds; ordinary grass-on appearance is assessed separately.' } |
   ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
if (!('TerrainPuddleMetrics' -as [type])) {
    $drawingRefs = @([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    # .NET 10 split Bitmap's native interfaces into these assemblies. Earlier
    # runtimes have no such references; derive them rather than pinning a version.
    $drawingRefs += @([Drawing.Bitmap].Assembly.GetReferencedAssemblies() |
        Where-Object Name -like 'System.Private.Windows.*' | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $drawingRefs
}
$p = $null; $client = $null; $writer = $null; $reader = $null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Installed runtime or mission failure; reject this run.'
    }
}
function Send($command) {
    Assert-RunHealth
    $request = $command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
    $writer.WriteLine($request)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Harness response deadline exceeded.' }
        $line = $reader.ReadLine()
        if ($null -eq $line) { throw 'Harness connection closed.' }
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
        $reply = $line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok) { throw $line }
    return $reply
}
function Simulation-Time {
    $reply = Send @{ cmd = 'eval'; code = 'time' }
    $value = 0.0
    if (![double]::TryParse(([string]$reply.result).Trim('"'), [Globalization.NumberStyles]::Float, $culture, [ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value)) { throw 'Harness returned invalid simulation time.' }
    return $value
}
function Screenshot([string]$name) {
    $path = Join-Path $output ($name + '.png')
    $null = Send @{ cmd = 'screenshot'; path = $path }
    $until = [DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {
        Assert-RunHealth
        if ([DateTime]::UtcNow -gt $until) { throw "Screenshot not written: $path" }
        Start-Sleep -Milliseconds 100
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG capture: $path" }
    return @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length }
}
function Pose-Capture([string]$name, [string[]]$tokens) {
    $reply = Send @{ cmd = 'eval'; code = ('triFreeFlyPose "' + ($tokens -join ' ') + '"') }
    if ($reply.result -notmatch 'OK') { throw "Camera refused: $name" }
    Start-Sleep -Seconds $SettleSeconds
    $capture = Screenshot $name
    $capture.pose = ($tokens -join ' '); $capture.simulationTime = Simulation-Time
    if ([Math]::Abs($capture.simulationTime - 100) -gt 0.001) { throw 'Paused simulation time changed.' }
    return $capture
}
function Uploaded-Wetness([double]$expected) {
    $rows = @(Select-String -LiteralPath $log -Pattern 'Wgpu terrain puddle weather: time=([\d.eE+-]+) rain=([\d.eE+-]+) accumulated=([\d.eE+-]+) uploaded=([\d.eE+-]+)')
    if (!$rows.Count) { throw 'No installed terrain wetness telemetry.' }
    $sample = $rows[-1]; $groups = $sample.Matches[0].Groups
    $uploaded = [double]::Parse($groups[4].Value,$culture)
    $time = [double]::Parse($groups[1].Value,$culture)
    if ([Math]::Abs($time-100) -gt 0.001 -or [Math]::Abs($uploaded-$expected) -gt 0.0001) {
        throw 'Installed terrain uploaded wetness or frozen time differs from the selected diagnostic arm.'
    }
    return @{ time = $time; uploaded = $uploaded; line = $sample.Line; lineNumber = $sample.LineNumber }
}
function Shift-Camera([string[]]$tokens, [double]$dx, [double]$dz, [double]$dh) {
    $copy = [string[]]$tokens.Clone()
    $copy[0] = ([double]::Parse($copy[0],$culture)+$dx).ToString('R',$culture)
    $copy[1] = ([double]::Parse($copy[1],$culture)+$dz).ToString('R',$culture)
    $copy[2] = ([double]::Parse($copy[2],$culture)+$dh).ToString('R',$culture)
    return $copy
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }
        $null = Send @{ cmd = 'exit' }
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
        if (!(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) { throw 'Clean shutdown marker missing.' }
    }
    if ($client) { $client.Dispose(); $script:client = $null }
    $script:writer = $null; $script:reader = $null
}
function Pair-Metrics($a,$b) { return [TerrainPuddleMetrics]::Compare($a.path,$b.path,$MaskThreshold) }
try {
    $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
    $env:WGR_TERRAIN_PUDDLE_FIXTURE = '1'; $env:WGR_TEMPORAL = '0'
    $env:WGR_AUTO_EXPOSURE = '0'; $env:WGR_EXPOSURE = '1'; $env:WGR_INTERIOR_SKY = '1'
    # Phase-integrated grass/wind retains pre-pause history across process starts.
    # Exclude grass from terrain-only pixel gates rather than treating vegetation
    # changes as puddle response or relaxing the independent master-off control.
    $env:WGR_GRASS = '0'; $env:POSEIDON_WIND_OVERRIDE = '0 90 0'
    $env:WGR_CLOUD_COVERAGE = '0'; $env:WGR_LOD_GOVERNOR_RANGE = '1'
    # Geography initializes terrain UV offsets from wall-clock-seeded GRandGen.
    # Native detail-noise still consumes tile_uv, so fixed time cannot reproduce
    # its phase across processes. Keep detail, but remove only UV displacement.
    $env:WGR_TERRAIN_JITTER = '0'
    . "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
    $env:POSEIDON_INTERIOR_SKY_PROBE = '3600'
    $armNames = @('dry','wet')
    foreach ($arm in $armNames) {
        if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Unexpected game process between arms.' }
        $armDir = Join-Path $output $arm; $armProfile = Join-Path $armDir 'user'
        New-Item -ItemType Directory -Force -Path $armProfile | Out-Null
        $log = Join-Path $armDir 'engine.log'
        $env:POSEIDON_USER_DIR = $armProfile
        $env:WGR_TERRAIN_PUDDLE_WETNESS = if ($arm -eq 'dry') { '0' } else { '1' }
        $env:WGR_TERRAIN_PUDDLES = if ($arm -eq 'master-off') { '0' } else { '1' }
        $env:WGR_INTERIOR_SKY_DEBUG = if ($arm -eq 'raw-sky') { '1' } else { '0' }
        [IO.File]::WriteAllText((Join-Path $armProfile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
            '--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$NativeAddons+'"'),
            '--test-world-hour','16','--test-world-freefly') + $numbers + @('--log-file',('"'+$log+'"'))
        $p = Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments
        $null = $p.Handle
        $until = [DateTime]::UtcNow.AddSeconds(120)
        do {
            Assert-RunHealth
            $client = [Net.Sockets.TcpClient]::new()
            try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client = $null }
            if (!$client) {
                if ([DateTime]::UtcNow -gt $until) { throw 'Harness unavailable after 120 seconds.' }
                Start-Sleep -Milliseconds 250
            }
        } while (!$client)
        $stream = $client.GetStream(); $stream.ReadTimeout = 30000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
        Start-Sleep -Seconds 12
        $ready = Send @{ cmd = 'eval'; code = 'triSceneReady' }
        if ($ready.result -notmatch 'OK') { throw 'Mission scene not ready.' }
        $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]' }
        foreach ($expression in @('triSetSimTime 100','triSetBrightness 1')) {
            $reply = Send @{ cmd = 'eval'; code = $expression }
            if ($reply.result -notmatch 'OK') { throw "Deterministic setup refused: $expression" }
        }
        $captures = @{}
        $fixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request } -X $RoofX -Z $RoofZ -ControlX $ControlX -ControlZ $ControlZ
        Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
        $covered = @(Camera-Numbers $fixture.coveredCamera)
        $exposed = @(Camera-Numbers $fixture.exposedCamera)
        $captures.absent = Pose-Capture ($arm+'-absent') $covered
        $fixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request } -X $RoofX -Z $RoofZ -ControlX $ControlX -ControlZ $ControlZ
        $captures.covered = Pose-Capture ($arm+'-covered') $covered
        # The late frame probe must fire while the known tent and covered camera
        # are active. Retained instance identity is separate from pixel proof.
        $probeUntil = [DateTime]::UtcNow.AddSeconds(180)
        do {
            Assert-RunHealth
            $probes = @(Select-String -LiteralPath $log -Pattern 'Interior sky probe at')
            if (!$probes.Count) { Start-Sleep -Milliseconds 250 }
            if ([DateTime]::UtcNow -gt $probeUntil) { throw 'Late retained-geometry probe did not fire.' }
        } while (!$probes.Count)
        $probeLine = $probes[-1].Line
        if ($probeLine -notmatch 'stan_eastc' -or $probeLine -notmatch ('at \['+[Math]::Round($RoofX)+' '+[Math]::Round($RoofZ-1.5)+' ')) {
            throw "Probe did not identify the actual roof at the covered camera: $probeLine"
        }
        $coverageUntil = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $coverage = @(Select-String -LiteralPath $log -Pattern 'interior sky maps \(requested\)')
            if (!$coverage.Count) { Start-Sleep -Milliseconds 100 }
            if ([DateTime]::UtcNow -gt $coverageUntil) { throw 'Directional depth coverage readback is missing.' }
        } while (!$coverage.Count)
        $captures.coveredStatic = Pose-Capture ($arm+'-covered-static') $covered
        $captures.exposed = Pose-Capture ($arm+'-exposed') $exposed
        Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
        $captures.removed = Pose-Capture ($arm+'-removed') $covered
        $fixture.retainedProbe = $probeLine
        $fixture.coverageProbe = $coverage[-1].Line
        $fixture.status = 'retained-captured-pixels-not-yet-accepted'
        $weather = Send @{ cmd = 'weather_visibility' }
        if ($weather.rain -gt 0.001 -or $weather.fog -gt 0.001) { throw 'Actual rain/fog invalidates fixed dry/wet image arms.' }
        $expectedWetness = if ($arm -eq 'dry' -or $arm -eq 'master-off') { 0.0 } else { 1.0 }
        $uploaded = Uploaded-Wetness $expectedWetness
        if (!(Select-String -LiteralPath $log -Pattern 'Wgpu: interior sky gate: enabled=true' -Quiet)) {
            throw 'Interior sky geometry map was not enabled in the installed renderer.'
        }
        $result.arms[$arm] = @{ pid = $p.Id; captures = $captures; actualWeather = $weather; log = $log;
            wetnessOverride = $env:WGR_TERRAIN_PUDDLE_WETNESS; master = $env:WGR_TERRAIN_PUDDLES; uploadedWetness = $uploaded; fixture = $fixture }
        Close-Owned
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Runtime failure invalidates captures.' }
    }
    $dry = $result.arms.dry.captures; $wet = $result.arms.wet.captures
    foreach ($view in @('absent','covered','coveredStatic','exposed','removed')) {
        $result.comparisons[$view] = @{ wetDry = Pair-Metrics $dry[$view] $wet[$view] }
    }
    $result.comparisons.absent.removedWet = Pair-Metrics $wet.absent $wet.removed
    $result.comparisons.covered.staticWet = Pair-Metrics $wet.covered $wet.coveredStatic
    $positive = $result.comparisons.absent.wetDry.ChangedFraction -ge 0.001 -and $result.comparisons.exposed.wetDry.ChangedFraction -ge 0.001
    $result.roof = @{ status = $(if ($positive) { 'retained-positive-controls-pixels-not-yet-accepted' } else { 'invalid-positive-control' }); positiveControls = $positive;
        reason = 'Inspect a common visible terrain ROI in absent/covered/removed views; central difference statistics include tent geometry.' }
    $result.status = 'captured'
} catch {
    $result.status = 'failed'; $result.error = $_.Exception.Message
    throw
} finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000); $result.forcedOwnedProcessStop = $true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after = Installed-State; $result.after = $after
        $changed = $after.deployedFrom -ne $before.deployedFrom
        for ($i = 0; $i -lt $before.files.Count; ++$i) {
            foreach ($field in @('name','path','sha256','bytes','writtenUtc')) {
                if ($after.files[$i][$field] -ne $before.files[$i][$field]) { $changed = $true }
            }
        }
        if ($changed) { $result.status = 'failed'; $result.error = 'Installed provenance or binaries changed during the campaign.' }
    } catch { $result.status = 'failed'; $result.error = $_.Exception.Message }
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    }
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Puddle stability evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host "Stock roof retained identity verified; pixel acceptance requires inspection of the recorded controls."
}
