[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Camera,
    [switch]$LegacyCwa,
    [ValidateSet('eden','noe')][string]$World = 'eden',
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'native-weather',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run this script through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$numbers = @($Camera.Trim() -split '\s+')
if ($numbers.Count -ne 5) { throw 'Camera must contain X Z absolute-height azimuth elevation.' }
foreach ($token in $numbers) {
    $value = 0.0
    if (![double]::TryParse($token, [Globalization.NumberStyles]::Float, $culture, [ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value)) { throw "Invalid camera number: $token" }
}
$root = Split-Path -Parent $PSScriptRoot
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
if ($World -ne 'eden' -and !$LegacyCwa) { throw 'Non-Everon mission requires LegacyCwa.' }
if ($World -eq 'noe') {
    $mission = Join-Path $root 'build/puddle-weather-fixture/perf_field.noe'
    New-Item -ItemType Directory -Force -Path $mission | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
}
$requiredPaths = @($mission)
if (!$LegacyCwa) { $requiredPaths += $NativeAddons }
foreach ($path in $requiredPaths) {
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
$privateProfile = Join-Path $output 'user'
New-Item -ItemType Directory -Force -Path $privateProfile | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; scope = 'installed rain growth/ripples/pause/partial-drying lifecycle; captures require visual acceptance';
    camera = ($numbers -join ' '); before = $before; samples = @{}; screenshots = @(); error = $null }
@{ scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash; sourceHead = (& git -C $root rev-parse HEAD);
   installed = $before; mission = $mission; legacyCwa = [bool]$LegacyCwa; world = $World; nativeAddons = $NativeAddons; camera = ($numbers -join ' ');
   lockOwner = $env:LOCK_OWNER; forcedRain = $false; wetnessOverride = $false } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$keys = @('POSEIDON_USER_DIR', 'POSEIDON_REFORGER_WORLD', 'POSEIDON_REFORGER_OBJECTS', 'POSEIDON_REFORGER_STREAM',
          'POSEIDON_TEST_RAIN', 'WGR_TERRAIN_PUDDLE_WETNESS', 'WGR_TERRAIN_PUDDLE_FIXTURE', 'WGR_TERRAIN_PUDDLES')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
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
function Weather-Sample {
    Assert-RunHealth
    $pattern = 'Wgpu terrain puddle weather: time=([\d.eE+-]+) rain=([\d.eE+-]+) accumulated=([\d.eE+-]+) uploaded=([\d.eE+-]+)'
    $rows = @(Select-String -LiteralPath $log -Pattern $pattern)
    if (!$rows.Count) { throw 'No installed puddle weather telemetry found; source-only evidence is insufficient.' }
    $last = $rows[-1]; $match = $last.Matches[0]
    $sample = @{ generation = $rows.Count; lineNumber = $last.LineNumber; line = $last.Line }
    $names = @('time', 'rain', 'accumulated', 'uploaded')
    for ($i = 0; $i -lt 4; ++$i) {
        $value = [double]::Parse($match.Groups[$i + 1].Value, $culture)
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { throw 'Nonfinite weather telemetry.' }
        $sample[$names[$i]] = $value
    }
    if ([Math]::Abs($sample.accumulated - $sample.uploaded) -gt 0.0002) { throw 'Accumulated wetness did not reach the uploaded terrain parameter.' }
    return $sample
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
try {
    $env:POSEIDON_USER_DIR = $privateProfile
    $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    if ($LegacyCwa) { Remove-Item Env:POSEIDON_REFORGER_WORLD, Env:POSEIDON_REFORGER_OBJECTS, Env:POSEIDON_REFORGER_STREAM -ErrorAction SilentlyContinue }
    Remove-Item Env:POSEIDON_TEST_RAIN, Env:WGR_TERRAIN_PUDDLE_WETNESS -ErrorAction SilentlyContinue
    $env:WGR_TERRAIN_PUDDLE_FIXTURE = '1'; $env:WGR_TERRAIN_PUDDLES = '1'
    [IO.File]::WriteAllText((Join-Path $privateProfile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $launchArguments = @('--render=wgpu', '--window', '--dev', '--width', '1280', '--height', '720', '--vd', '2000',
        '--harness', "$port", '--test-mission', ('"' + $mission + '"'))
    if (!$LegacyCwa) { $launchArguments += @('--test-world', ('"' + $NativeAddons + '"')) }
    $launchArguments += @('--test-world-hour', '16', '--test-world-freefly') + $numbers + @('--log-file', ('"' + $log + '"'))
    $p = Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $launchArguments
    $null = $p.Handle; $result.processId = $p.Id
    $until = [DateTime]::UtcNow.AddSeconds(120)
    do {
        Assert-RunHealth
        $client = [Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1', $port) } catch { $client.Dispose(); $client = $null }
        if (!$client) {
            if ([DateTime]::UtcNow -gt $until) { throw 'Owned harness did not become available in 120 seconds.' }
            Start-Sleep -Milliseconds 250
        }
    } while (!$client)
    $stream = $client.GetStream(); $stream.ReadTimeout = 30000
    $reader = [IO.StreamReader]::new($stream)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    Start-Sleep -Seconds 12
    $pose = Send @{ cmd = 'eval'; code = ('triFreeFlyPose "' + ($numbers -join ' ') + '"') }
    if ($pose.result -notmatch 'OK') { throw 'Candidate camera pose refused.' }
    $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 1' }
    Start-Sleep -Seconds 2
    $result.screenshots += Screenshot 'dry'
    $result.samples.dry = Weather-Sample
    $null = Send @{ cmd = 'exec'; code = '0 setOvercast 1; 0 setRain 1; setAccTime 3' }
    $rainStart = Simulation-Time
    Start-Sleep -Seconds 5
    $result.screenshots += Screenshot 'growing'
    $result.samples.growing = Weather-Sample
    Start-Sleep -Seconds 10
    $result.screenshots += Screenshot 'wet'
    $wet = Weather-Sample; $result.samples.wet = $wet
    # Ordinary weather interpolates toward its next forecast even after an
    # immediate setRain. Require meaningful actual rain rather than a frozen
    # full-rain override; recorded samples retain the actual observed density.
    if ($wet.time -le $rainStart -or $wet.rain -lt 0.3 -or $wet.accumulated -le 0.3) { throw 'Actual rain did not fill terrain wetness above 0.3.' }
    if ($wet.accumulated -le $result.samples.growing.accumulated) { throw 'Puddle water history did not continue growing in real rain.' }
    $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }
    for ($i=0; $i -lt 4; ++$i) {
        Start-Sleep -Milliseconds 200
        $result.screenshots += Screenshot ('rain-ripple-' + $i)
    }
    $null = Send @{ cmd = 'exec'; code = 'setAccTime 0' }
    Start-Sleep -Seconds 1
    $pauseTimeA = Simulation-Time
    $result.screenshots += Screenshot 'paused-a'
    $pauseA = Weather-Sample
    Start-Sleep -Seconds 5
    $pauseTimeB = Simulation-Time
    $result.screenshots += Screenshot 'paused-b'
    $pauseB = Weather-Sample
    $result.samples.pause = @{ simulationTimeA = $pauseTimeA; simulationTimeB = $pauseTimeB; before = $pauseA; after = $pauseB }
    if ([Math]::Abs($pauseTimeB - $pauseTimeA) -gt 0.001 -or $pauseA.generation -ne $pauseB.generation -or
        $pauseA.accumulated -ne $pauseB.accumulated -or $pauseA.uploaded -ne $pauseB.uploaded) { throw 'Simulation time or puddle telemetry advanced while paused.' }
    $null = Send @{ cmd = 'exec'; code = '0 setOvercast 0; 0 setRain 0; setAccTime 3' }
    $dryStart = Simulation-Time
    Start-Sleep -Seconds 12
    $result.screenshots += Screenshot 'drying'
    $dry = Weather-Sample; $result.samples.drying = $dry
    if ($dry.time -le $dryStart -or $dry.rain -gt 0.02 -or $dry.accumulated -ge $pauseB.accumulated - 0.005) { throw 'Actual rain did not stop or accumulated wetness did not measurably decrease.' }
    $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }
    $null = Send @{ cmd = 'exit' }
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
    if (!(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) { throw 'Clean shutdown marker missing.' }
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Runtime error invalidates lifecycle evidence.' }
    $result.exitCode = $p.ExitCode; $result.status = 'ok'
} catch {
    $result.status = 'failed'; $result.error = $_.Exception.Message
    throw
} finally {
    if ($p -and !$p.HasExited) {
        if ($writer) {
            try { $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }; $null = Send @{ cmd = 'exit' }; $null = $p.WaitForExit(5000) } catch {}
        }
        if (!$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000); $result.forcedOwnedProcessStop = $true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after = Installed-State; $result.after = $after
        $changed = $after.deployedFrom -ne $before.deployedFrom
        for ($i = 0; $i -lt $before.files.Count; ++$i) {
            foreach ($field in @('name', 'path', 'sha256', 'bytes', 'writtenUtc')) {
                if ($after.files[$i][$field] -ne $before.files[$i][$field]) { $changed = $true }
            }
        }
        if ($changed) {
            $result.status = 'failed'; $result.error = 'Installed stamp, EXE or DLL changed during the lifecycle run.'
        }
    } catch { $result.status = 'failed'; $result.error = $_.Exception.Message }
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], 'Process') }
    $result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Terrain puddle lifecycle evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host ("Actual weather samples: wet rain={0:F3} wetness={1:F4}; paused simtime={2:F3}/{3:F3}; drying rain={4:F3} wetness={5:F4}" -f
        $result.samples.wet.rain, $result.samples.wet.accumulated, $result.samples.pause.simulationTimeA,
        $result.samples.pause.simulationTimeB, $result.samples.drying.rain, $result.samples.drying.accumulated)
}
