# Installed grounded rigid stock-model falsifier; config-only private class.
# Run only through with-game-lock.sh. No built-in stock class is claimed.
[CmdletBinding()]
param(
    [double]$X=9486, [double]$Z=3006,
    [string]$Camera='9481.25 2994.25 209.72 0 -18',
    [ValidateRange(1,15)][int]$SettleSeconds=3,
    [ValidateRange(1,64)][int]$MaskThreshold=8,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='ground-rigid',
    [string]$Python='python',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
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
$result = [ordered]@{ status = 'running'; scope = 'ground-level rigid stock-model source admission and isolated dry/wet/master-off pixel controls';
    before = $before; arms = @{}; comparisons = @{}; gates = @{}; error = $null;
    fixture = @{ status = 'not-captured' } }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLES',
    'WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_INTERIOR_SKY','WGR_INTERIOR_SKY_DEBUG',
    'POSEIDON_INTERIOR_SKY_PROBE','WGR_GROUND_RECEIVER_TRACE','WGR_WEATHER_COVER','WGR_WEATHER_COVER_TRACE','WGR_OBJECT_SNOW','POSEIDON_TEST_SNOW','WGR_GRASS','POSEIDON_WIND_OVERRIDE','WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
$auditScript=Join-Path $PSScriptRoot 'Inspect-GroundMeshStockFixture.py'
# A read-only retail audit generates a config-only private addon under output.
# It refuses changed/missing source assets before any game launch.
& $Python $auditScript --game-dir $GameDir --out $output | Out-File -LiteralPath (Join-Path $output 'audit.stdout.txt')
if ($LASTEXITCODE -ne 0) {
    $result.status='fixture-unavailable';$result.error='Bounded stock source audit failed. No game launched.'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    throw ('Fixture unavailable: '+$result.error)
}
$source=Get-Content -LiteralPath (Join-Path $output 'stock-source.json') -Raw | ConvertFrom-Json
$result.fixture.source=$source
@{scriptSha256=(Get-FileHash $PSCommandPath).Hash;
  helperSha256=(Get-FileHash (Join-Path $PSScriptRoot 'GroundMeshConcreteFixture.ps1')).Hash;
  auditSha256=(Get-FileHash $auditScript).Hash;
  metricsSha256=(Get-FileHash (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs')).Hash;
  sourceHead=(& git -C $root rev-parse HEAD);installed=$before;mission=$mission;
  nativeAddons=$NativeAddons;lockOwner=$env:LOCK_OWNER;source=$source;
  centre=@($X,$Z);fixedSimTime=100;fixedDate=@(1985,6,21,16,0);
  wetnessOverrides=@(0,1,1);actualLiquidRain=0;temporal=0;exposure=1;grass=0;wind='0 90 0';
  cloud=0;jitter=0;lodRange=1;physicalWeatherMap=1;maskThreshold=$MaskThreshold;roi='central 60% width and height';
  scope='private alias using untouched stock model; runtime source admission and visible ROI require evidence';
  gaps=@('no named stock pavement class','no cutout positive','no rain-hit ripple proof','no all-map coverage claim')} |
  ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
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
function Receiver-Proof($fixture) {
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do {
        Assert-RunHealth
        $rows=@(Select-String -LiteralPath $log -Pattern 'Wgpu ground mesh receiver: model=.*molo_beton.* path=.* eligible=true static=true x=([\d.eE+-]+) y=([\d.eE+-]+) z=([\d.eE+-]+)')
        if (!$rows.Count) {Start-Sleep -Milliseconds 100}
        if ([DateTime]::UtcNow -gt $deadline) {throw 'Fixture unavailable: installed concrete receiver source proof is absent.'}
    } while (!$rows.Count)
    # The source trace is deduplicated per model/path/admission, so require its
    # FIRST placement to match the actual seated object. Later raises carry
    # their independently queried poses; they do not pretend to emit new tags.
    $row=$rows[-1];$g=$row.Matches[0].Groups
    $logged=@([double]::Parse($g[1].Value,$culture),[double]::Parse($g[3].Value,$culture),[double]::Parse($g[2].Value,$culture))
    for ($i=0;$i -lt 3;$i++) {
        if ([Math]::Abs($logged[$i]-$fixture.pose.actualPosition[$i]) -gt .02) {
            throw 'Fixture unavailable: deduplicated source trace is not the actual seated concrete pose.'
        }
    }
    return @{line=$row.Line;lineNumber=$row.LineNumber;loggedPosition=$logged;scope='source proof, not fragment height/rain or appearance proof'}
}
try {
    $env:POSEIDON_REFORGER_WORLD='worlds/eden';$env:POSEIDON_REFORGER_OBJECTS='1';$env:POSEIDON_REFORGER_STREAM='1'
    Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
    Remove-Item Env:POSEIDON_TEST_SNOW -ErrorAction SilentlyContinue
    $env:WGR_TERRAIN_PUDDLE_FIXTURE='1';$env:WGR_GROUND_RECEIVER_TRACE='1'
    $env:WGR_WEATHER_COVER='1';$env:WGR_WEATHER_COVER_TRACE='1'
    $env:WGR_TEMPORAL='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='1'
    $env:WGR_INTERIOR_SKY='1';$env:WGR_INTERIOR_SKY_DEBUG='0';$env:WGR_OBJECT_SNOW='0'
    $env:WGR_GRASS='0';$env:POSEIDON_WIND_OVERRIDE='0 90 0';$env:WGR_CLOUD_COVERAGE='0'
    $env:WGR_LOD_GOVERNOR_RANGE='1';$env:WGR_TERRAIN_JITTER='0';$env:POSEIDON_INTERIOR_SKY_PROBE='3600'
    . "$PSScriptRoot/GroundMeshConcreteFixture.ps1"
    foreach ($arm in @('dry','wet','master-off')) {
        if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Unexpected game between owned arms.'}
        $armDir=Join-Path $output $arm;$profile=Join-Path $armDir 'user'
        New-Item -ItemType Directory -Force -Path $profile | Out-Null
        $log=Join-Path $armDir 'engine.log';$env:POSEIDON_USER_DIR=$profile
        $env:WGR_TERRAIN_PUDDLE_WETNESS=if ($arm -eq 'dry') {'0'} else {'1'}
        $env:WGR_TERRAIN_PUDDLES=if ($arm -eq 'master-off') {'0'} else {'1'}
        [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
        $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
            '--mod',('"'+$source.privateMod+'"'),'--harness',"$port",'--test-mission',('"'+$mission+'"'),
            '--test-world',('"'+$NativeAddons+'"'),'--test-world-hour','16','--test-world-freefly')+$numbers+@('--log-file',('"'+$log+'"'))
        $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments
        $null=$p.Handle;$until=[DateTime]::UtcNow.AddSeconds(120)
        do {
            Assert-RunHealth;$client=[Net.Sockets.TcpClient]::new()
            try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null}
            if (!$client) {
                if ([DateTime]::UtcNow -gt $until) {throw 'Harness unavailable after 120 seconds.'}
                Start-Sleep -Milliseconds 250
            }
        } while (!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000
        $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        Start-Sleep -Seconds 12
        $ready=Send @{cmd='eval';code='triSceneReady'}
        if ($ready.result -notmatch 'OK') {throw 'Mission scene not ready.'}
        $null=Send @{cmd='exec';code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]'}
        foreach ($code in @('triSetSimTime 100','triSetBrightness 1')) {
            $reply=Send @{cmd='eval';code=$code}
            if ($reply.result -notmatch 'OK') {throw "Deterministic setup refused: $code"}
        }
        $plan=Get-GroundMeshConcretePlan -SendCommand {param($q) Send $q} -X $X -Z $Z -ModelTopY $source.modelTopY
        $fixture=New-GroundMeshConcreteFixture -SendCommand {param($q) Send $q} -Plan $plan
        $captures=@{};$poses=@{grounded=$fixture.pose}
        $groundCamera=@(Camera-Numbers (Get-GroundMeshConcreteCamera $plan))
        $captures.exposed=Pose-Capture ($arm+'-exposed') $groundCamera
        $fixture.receiverProof=Receiver-Proof $fixture
        $captures.static=Pose-Capture ($arm+'-static') $groundCamera
        $moveCamera=Shift-Camera $groundCamera 3 0 1
        $captures.moved=Pose-Capture ($arm+'-moved') $moveCamera
        $captures.returned=Pose-Capture ($arm+'-returned') $groundCamera
        $roof=New-TerrainPuddleStockRoof -SendCommand {param($q) Send $q} -X $X -Z $Z -RoofLift 3
        $captures.covered=Pose-Capture ($arm+'-covered') $groundCamera
        # Wait for actual retained roof identity/depth readback at this camera.
        $deadline=[DateTime]::UtcNow.AddSeconds(180)
        do {
            Assert-RunHealth
            $probes=@(Select-String -LiteralPath $log -Pattern 'Interior sky probe at')
            $coverage=@(Select-String -LiteralPath $log -Pattern 'interior sky maps \(requested\)')
            if (!$probes.Count -or !$coverage.Count) {Start-Sleep -Milliseconds 100}
            if ([DateTime]::UtcNow -gt $deadline) {throw 'Fixture unavailable: actual roof depth readback absent.'}
        } while (!$probes.Count -or !$coverage.Count)
        if ($probes[-1].Line -notmatch 'stan_eastc') {throw 'Fixture unavailable: actual retained roof identity not proven.'}
        $roof.retainedProbe=$probes[-1].Line;$roof.coverageProbe=$coverage[-1].Line
        $captures.covered=Pose-Capture ($arm+'-covered-ready') $groundCamera
        Remove-TerrainPuddleStockRoof -SendCommand {param($q) Send $q}
        $captures.removed=Pose-Capture ($arm+'-removed') $groundCamera
        $poses.raised=Set-GroundMeshConcretePose -SendCommand {param($q) Send $q} -Plan $plan -Raise 1
        $captures.raised=Pose-Capture ($arm+'-raised') @(Camera-Numbers (Get-GroundMeshConcreteCamera $plan -Raise 1))
        $poses.underside=Set-GroundMeshConcretePose -SendCommand {param($q) Send $q} -Plan $plan -Raise 8
        $captures.underside=Pose-Capture ($arm+'-underside') @(Camera-Numbers (Get-GroundMeshConcreteCamera $plan -Underside))
        $poses.restored=Set-GroundMeshConcretePose -SendCommand {param($q) Send $q} -Plan $plan
        $captures.restored=Pose-Capture ($arm+'-restored') $groundCamera
        Remove-GroundMeshConcreteFixture -SendCommand {param($q) Send $q}
        $weather=Send @{cmd='weather_visibility'}
        if ($weather.rain -gt .001 -or $weather.fog -gt .001) {throw 'Actual rain/fog invalidates fixed diagnostic arms.'}
        $uploaded=Uploaded-Wetness $(if ($arm -eq 'dry' -or $arm -eq 'master-off') {0.0} else {1.0})
        $result.arms[$arm]=@{pid=$p.Id;log=$log;captures=$captures;fixture=$fixture;poses=$poses;roof=$roof;weather=$weather;uploadedWetness=$uploaded;arguments=$arguments}
        Close-Owned
    }
    $d=$result.arms.dry.captures;$w=$result.arms.wet.captures;$off=$result.arms['master-off'].captures
    foreach ($view in @('exposed','static','returned','covered','removed','raised','underside','restored')) {
        $result.comparisons[$view]=@{wetDry=Pair-Metrics $d[$view] $w[$view];offDry=Pair-Metrics $d[$view] $off[$view]}
    }
    $result.comparisons.staticWet=Pair-Metrics $w.exposed $w.static
    $result.comparisons.returnMask=[TerrainPuddleMetrics]::MaskOverlap($d.exposed.path,$w.exposed.path,$d.returned.path,$w.returned.path,$MaskThreshold)
    $result.comparisons.removedWet=Pair-Metrics $w.exposed $w.removed
    $result.comparisons.restoredWet=Pair-Metrics $w.exposed $w.restored
    $result.gates.source=@{passed=$true;scope='actual class/model/source tag plus seated world pose required in each arm'}
    $result.gates.positive=$result.comparisons.exposed.wetDry.ChangedFraction -ge .001
    $result.gates.returnMask=$result.comparisons.returnMask.NonEmpty -and $result.comparisons.returnMask.IoU -ge .97
    $result.gates.paused=$result.comparisons.staticWet.MeanAbsChannel -le .5 -and $result.comparisons.staticWet.ChangedFraction -le .005
    foreach ($view in @('covered','raised','underside')) {
        $m=$result.comparisons[$view].wetDry
        $result.gates[$view]=$m.MeanAbsChannel -le .5 -and $m.ChangedFraction -le .005
    }
    $result.gates.masterOff=$true
    foreach ($view in @('exposed','covered','raised','underside')) {
        $m=$result.comparisons[$view].offDry
        if ($m.MeanAbsChannel -gt .5 -or $m.ChangedFraction -gt .005) {$result.gates.masterOff=$false}
    }
    foreach ($name in @('removedWet','restoredWet')) {
        $m=$result.comparisons[$name]
        $result.gates[$name]=$m.MeanAbsChannel -le .5 -and $m.ChangedFraction -le .005
    }
    $failed=@($result.gates.Keys | Where-Object { $result.gates[$_] -is [bool] -and !$result.gates[$_] })
    if ($failed.Count) {throw ('Numeric controls failed: '+($failed -join ', ')+'; retain captures and inspect actual concrete ROI. Do not relax thresholds.')}
    $result.status='numeric-controls-pass-appearance-pending'
    $result.fixture=$result.arms.wet.fixture
} catch {
    $result.error=$_.Exception.Message
    $result.status=if ($result.error -like 'Fixture unavailable:*') {'fixture-unavailable'} else {'failed'}
    throw
} finally {
    if ($p -and !$p.HasExited) {
        try {Close-Owned} catch {}
        if (!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedOwnedProcessStop=$true}
    }
    if ($client) {$client.Dispose()}
    try {
        $after=Installed-State;$result.after=$after
        $changed=$after.deployedFrom -ne $before.deployedFrom
        for ($i=0;$i -lt $before.files.Count;$i++) {
            foreach ($field in @('name','path','sha256','bytes','writtenUtc')) {
                if ($after.files[$i][$field] -ne $before.files[$i][$field]) {$changed=$true}
            }
        }
        if ($changed) {$result.status='failed';$result.error='Installed provenance/binaries changed during campaign.'}
    } catch {$result.status='failed';$result.error=$_.Exception.Message}
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) {Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}
        else {[Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process')}
    }
    $result | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Grounded rigid mesh evidence: $output"
    if ($result.status -in @('failed','fixture-unavailable')) {throw $result.error}
    Write-Host 'Numeric controls passed; visible concrete ROI and appearance still require inspection. No all-map or cutout acceptance claim.'
}
