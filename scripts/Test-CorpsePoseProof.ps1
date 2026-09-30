[CmdletBinding()]
param(
    [ValidateRange(8,120)][int]$CaptureDeadlineSeconds = 60,
    [ValidateRange(1,4)][double]$ExpiryAcceleration = 4,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'installed-proof',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
function Installed-State {
    # First installed file read: preserve the stamped executable/DLL provenance.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Installed deployment provenance is empty.' }
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
$before = Installed-State
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in @($mission,$NativeAddons)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required native fixture path missing: $path" }
}
$output = Join-Path $root ('build/corpse-pose/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0,6))
$profile = Join-Path $output 'user'
New-Item -ItemType Directory -Force -Path $profile | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; scope = 'installed no-motion affine pose and lifecycle proof; no solver or appearance acceptance';
    before = $before; gates = @{}; observations = @{}; captures = @{}; error = $null;
    saveNetwork = 'Not exercised by this runner; no save/network fields were added.' }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GRASS','POSEIDON_WIND_OVERRIDE',
    'WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
@{ scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash; sourceHead = (& git -C $root rev-parse HEAD);
   installed = $before; mission = $mission; nativeAddons = $NativeAddons; lockOwner = $env:LOCK_OWNER;
   stockClass = 'SoldierWB'; spawn = 'getPos player, group player, assigned via createUnit init';
   camera = 'actual getPosASL corpse, four metres behind and 2.7 metres above';
   fixedDate = @(1985,6,21,16,0); pausedImageComparison = $true; captureDeadlineSeconds = $CaptureDeadlineSeconds;
   expiryAcceleration = $ExpiryAcceleration; temporal = 0; exposure = 1; grass = 0; wind = '0 90 0';
   gates = @('live-refusal','phase-consumer-equivalence','status-identity','explicit-clear','30-second-expiry','hide-invalidation','delete-invalidation') } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
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
function Eval([string]$code) {
    $reply = Send @{ cmd = 'eval'; code = $code }
    if ($null -eq $reply.result) { throw "Evaluator result missing: $code" }
    # reply.result is an SQF value display, not nested JSON. Strings retain
    # literal model-path backslashes and escape embedded quotes by doubling them.
    $display = ([string]$reply.result).Trim()
    if ($display.StartsWith('"')) {
        if ($display.Length -lt 2 -or !$display.EndsWith('"')) { throw "Malformed SQF string: $code" }
        $inner = $display.Substring(1,$display.Length-2)
        $decoded = [Text.StringBuilder]::new()
        for ($i = 0; $i -lt $inner.Length; ++$i) {
            if ($inner[$i] -eq '"') {
                if ($i+1 -ge $inner.Length -or $inner[$i+1] -ne '"') { throw "Malformed doubled SQF quote: $code" }
                ++$i
            }
            $null = $decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Query([string]$command) {
    $value = Eval ('triPhysicsShowcase "' + $command + '"')
    if ($value -isnot [string] -or $value -notmatch '^(OK|REFUSED|MISMATCH) corpse-') {
        throw "Missing or unknown corpse diagnostic response for $command`: $value"
    }
    return $value
}
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Simulation-Time {
    $value = Eval 'time'
    $number = 0.0
    if (![double]::TryParse([string]$value,[Globalization.NumberStyles]::Float,$culture,[ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'Invalid simulation time.' }
    return $number
}
function Set-Acceleration([double]$value) {
    $null = Send @{ cmd = 'exec'; code = ('setAccTime ' + $value.ToString('R',$culture)) }
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
    if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG: $path" }
    return @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length; simulationTime = Simulation-Time }
}
function Position-Camera {
    $position = Eval 'getPosASL poseProofCorpse'
    Require ($position -is [array] -and $position.Count -eq 3) 'Corpse getPosASL did not return three coordinates.'
    foreach ($coordinate in $position) {
        Require (![double]::IsNaN([double]$coordinate) -and ![double]::IsInfinity([double]$coordinate)) 'Corpse position is nonfinite.'
    }
    $camera = @([double]$position[0],([double]$position[1]-4),([double]$position[2]+2.7),0,-34)
    $tokens = @($camera | ForEach-Object { ([double]$_).ToString('R',$culture) })
    $reply = Eval ('triFreeFlyPose "' + ($tokens -join ' ') + '"')
    Require ($reply -ceq 'OK') "Actual-corpse camera refused: $reply"
    $result.observations.corpsePosition = $position
    $result.observations.camera = $tokens -join ' '
    Start-Sleep -Seconds 2
}
function Spawn-Stock {
    $null = Send @{ cmd = 'exec'; code = '"SoldierWB" createUnit [getPos player,group player,"poseProofCorpse=this; removeAllWeapons this; this disableAI ""MOVE""; this setCombatMode ""BLUE"""]; poseProofCorpse setDir 0' }
    Require ((Eval 'isNull poseProofCorpse') -eq $false) 'Stock createUnit did not assign the fixture.'
    Require ((Eval 'poseProofCorpse == player') -eq $false) 'Fixture identity unexpectedly equals player.'
    Require ((Eval 'typeOf poseProofCorpse') -ceq 'SoldierWB') 'Spawned class is not the stock SoldierWB fixture.'
    Require ((Eval 'alive poseProofCorpse') -eq $true) 'Spawned stock fixture is not alive.'
}
function Wait-Capture {
    $until = [DateTime]::UtcNow.AddSeconds($CaptureDeadlineSeconds)
    do {
        $capture = Query 'corpse-capture'
        $result.observations.lastAdmission = $capture
        if ($capture -match '^OK corpse-capture entity=\S+ model=.+ distance=[\d.eE+-]+ nearbyCorpses=\d+ eligible=\d+ bones=\d+ primaryMatrices=\d+ secondaryMatrices=\d+ expiresMs=30000 solver=none rootMotion=authored$') { return $capture }
        Require ($capture -match '^REFUSED corpse-capture .*reason=(settled-death-pose-required|dry-ground-contact-required|no-nearby-corpse)$') "Stock capture failed a non-transient admission gate: $capture"
        if ([DateTime]::UtcNow -gt $until) { throw "Stock corpse never met the unchanged capture gate: $capture" }
        Start-Sleep -Milliseconds 500
    } while ($true)
}
function Assert-Compare([string]$name) {
    $compare = Query 'corpse-compare'
    Require ($compare -match '^OK corpse-compare .*paletteEqual=1 .*pointMismatch=0 .*proxyMismatch=0 holdValid=1$') "Production consumer equivalence failed: $compare"
    Require ($compare -match ' points=([1-9]\d*) ') "No actual point consumers compared: $compare"
    Require ($compare -match ' proxies=([1-9]\d*) ') "No actual proxy consumers compared: $compare"
    Require ($compare -match ' maxPointError=0 ') "No-motion point error differs from zero: $compare"
    $result.gates[$name] = @{ passed = $true; actual = $compare }
}
function Require-NoHold([string]$name) {
    $status = Query 'corpse-status'
    Require ($status -ceq 'REFUSED corpse-status no-valid-hold') "Hold survived $name`: $status"
    $result.gates[$name] = @{ passed = $true; actual = $status; simulationTime = Simulation-Time }
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        Set-Acceleration 1
        $null = Send @{ cmd = 'exit' }
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
        if (!(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) { throw 'Clean shutdown marker missing.' }
    }
    if ($client) { $client.Dispose(); $script:client = $null }
    $script:writer = $null; $script:reader = $null
}
try {
    $env:POSEIDON_USER_DIR = $profile
    $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
    $env:WGR_TEMPORAL = '0'; $env:WGR_AUTO_EXPOSURE = '0'; $env:WGR_EXPOSURE = '1'
    $env:WGR_GRASS = '0'; $env:POSEIDON_WIND_OVERRIDE = '0 90 0'; $env:WGR_CLOUD_COVERAGE = '0'
    $env:WGR_LOD_GOVERNOR_RANGE = '1'; $env:WGR_TERRAIN_JITTER = '0'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
        '--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$NativeAddons+'"'),
        '--test-world-hour','16','--test-world-freefly','6532','6466','100','0','-30','--log-file',('"'+$log+'"'))
    $result.launchArguments = $arguments
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
    Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission scene not ready.'
    Require ((Eval 'alive player') -eq $true) 'Fixture mission has no living player.'
    $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setDate [1985,6,21,16,0]; setAccTime 1' }
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=0') 'Unexpected default active hold.'
    Require-NoHold 'defaultInactive'
    Require ((Query 'corpse-not-a-command') -ceq 'REFUSED corpse-command unknown') 'Unknown command did not fail explicitly.'
    Spawn-Stock
    Position-Camera
    $live = Query 'corpse-capture'
    Require ($live -match '^REFUSED corpse-capture .*nearbyCorpses=0 eligible=0 reason=no-nearby-corpse$') "Living fixture was admitted or unrelated nearby corpse exists: $live"
    $result.gates.liveRefusal = @{ passed = $true; actual = $live }
    $null = Send @{ cmd = 'exec'; code = 'poseProofCorpse setDammage 1' }
    $null = Wait-Capture
    Require ((Eval 'alive poseProofCorpse') -eq $false) 'Capture accepted a living fixture.'
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=1') 'Readiness capture did not clear.'
    Set-Acceleration 0
    Position-Camera
    $result.captures.before = Screenshot 'before-held'
    $capture = Wait-Capture
    $status = Query 'corpse-status'
    Require ($status -match '^OK corpse-status .*remainingMs=30000$') "Fresh paused lease status invalid: $status"
    $identity = [regex]::Match($capture,'entity=(\S+)').Groups[1].Value
    Require ($status -match ('entity=' + [regex]::Escape($identity) + ' ')) "Capture/status owner identity changed: $capture / $status"
    $result.gates.statusIdentity = @{ passed = $true; capture = $capture; status = $status }
    Assert-Compare 'consumerEquivalence'
    $result.captures.held = Screenshot 'held'
    Require ($result.captures.before.simulationTime -eq $result.captures.held.simulationTime) 'Before/held screenshots advanced simulation.'
    $clear = Query 'corpse-clear'
    Require ($clear -ceq 'OK corpse-clear cleared=1') "Explicit clear failed: $clear"
    Require-NoHold 'explicitClear'
    $result.captures.cleared = Screenshot 'cleared'
    $result.observations.expiryCapture = Wait-Capture
    $expiryStart = Simulation-Time
    Set-Acceleration $ExpiryAcceleration
    $until = [DateTime]::UtcNow.AddSeconds(50)
    do {
        Start-Sleep -Milliseconds 500
        $expiryStatus = Query 'corpse-status'
        $expiryNow = Simulation-Time
        if ($expiryStatus -ceq 'REFUSED corpse-status no-valid-hold') { break }
        Require ($expiryStatus -match '^OK corpse-status ') "Unexpected expiry status: $expiryStatus"
        if ([DateTime]::UtcNow -gt $until) { throw 'Lease did not expire after simulation progressed.' }
    } while ($true)
    Require ($expiryNow - $expiryStart -gt 30) "Hold cleared before its expiry: elapsed=$($expiryNow-$expiryStart), $expiryStatus"
    Set-Acceleration 0
    $result.gates.expiry = @{ passed = $true; start = $expiryStart; end = $expiryNow; actual = $expiryStatus; acceleration = $ExpiryAcceleration }
    $result.observations.hideCapture = Wait-Capture
    $null = Send @{ cmd = 'exec'; code = 'poseProofHidden=poseProofCorpse; player action ["HIDEBODY",poseProofCorpse]; setAccTime 1' }
    $until = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 250
        $hideStatus = Query 'corpse-status'
        if ($hideStatus -ceq 'REFUSED corpse-status no-valid-hold') { break }
        Require ($hideStatus -match '^OK corpse-status ') "Unexpected hide status: $hideStatus"
        if ([DateTime]::UtcNow -gt $until) { throw "HIDEBODY did not invalidate the hold: $hideStatus" }
    } while ($true)
    Set-Acceleration 0
    Require-NoHold 'hideInvalidation'
    $hideRefusal = Query 'corpse-capture'
    Require ($hideRefusal -match '^REFUSED corpse-capture .*reason=(hidden-or-hiding|no-nearby-corpse|removed-or-deleting)$') "Hidden fixture capture was not explicitly refused: $hideRefusal"
    $result.gates.hideInvalidation.captureRefusal = $hideRefusal
    $result.captures.hidden = Screenshot 'hide-invalidated'
    Set-Acceleration 1
    Spawn-Stock
    Position-Camera
    $null = Send @{ cmd = 'exec'; code = 'poseProofCorpse setDammage 1' }
    $result.observations.deleteCapture = Wait-Capture
    Assert-Compare 'secondCorpseEquivalence'
    $null = Send @{ cmd = 'exec'; code = 'deleteVehicle poseProofCorpse' }
    Require-NoHold 'deleteInvalidation'
    $null = Send @{ cmd = 'exec'; code = 'deleteVehicle poseProofHidden' }
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=0') 'Cleanup found a surviving lease.'
    Require ((Eval 'alive player') -eq $true) 'Runner damaged or deleted the player.'
    Close-Owned
    $result.status = 'passed'
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
    Write-Host "Corpse pose evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host 'Installed shared affine pose/lifecycle gates passed; screenshots remain available for appearance review.'
}
