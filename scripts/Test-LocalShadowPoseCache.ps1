param(
    [switch]$ForceRefresh,
    [switch]$Disabled,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label = 'local-shadow-pose',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) {
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $env:LOCK_OWNER = 'Bounded original local shadow pose fixture'
    try {
        $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe, '-NoProfile', '-File', $PSCommandPath, '-Label', $Label, '-GameDir', $GameDir)
        if ($ForceRefresh) { $argv += '-ForceRefresh' }
        if ($Disabled) { $argv += '-Disabled' }
        & 'C:\Program Files\Git\bin\bash.exe' @argv
        if ($LASTEXITCODE) { throw "Local shadow fixture exited $LASTEXITCODE" }
    } finally { Remove-Item Env:LOCK_OWNER }
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root ('build/stream-residency/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss))
$profile = Join-Path $out 'user'; New-Item -ItemType Directory -Force -Path $profile | Out-Null
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
[IO.File]::WriteAllText((Join-Path $profile 'game.cfg'), "preferredViewDistance=2000;`n")
$mission = Join-Path $root 'dev-missions/freefly-lights.eden'
$exe = Join-Path $GameDir 'OpenPoseidon.exe'; $dll = Join-Path $GameDir 'wgpu_renderer.dll'
$log = Join-Path $out 'engine.log'; $stdout = Join-Path $out 'stdout.txt'; $stderr = Join-Path $out 'stderr.txt'
$environment = @{
    POSEIDON_USER_DIR = $profile; WGR_AUTO_EXPOSURE = '0'; WGR_EXPOSURE = '2'
    WGR_LOCAL_SHADOW_CACHE = '1'; WGR_LOCAL_SHADOW_POSE_CACHE = $(if ($Disabled) { '0' } else { '1' }); WGR_LOCAL_POSE_CACHE_TRACE = '1'
    POSEIDON_LOCAL_SHADOW_FORCE_REFRESH = $(if ($ForceRefresh) { '1' } else { '0' })
}
$clearNames = @((Get-ChildItem Env: | Where-Object { $_.Name -like 'WGR_*' -or $_.Name -like 'POSEIDON_*' } | ForEach-Object { $_.Name }))
$changedNames = @($clearNames + @($environment.Keys) | Select-Object -Unique)
$savedEnvironment = @{}; foreach ($name in $changedNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
$p = $null; $client = $null; $completed = $false
try {
    foreach ($name in $clearNames) { Remove-Item ('Env:' + $name) -ErrorAction SilentlyContinue }
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $hashes = @(Get-FileHash $exe, $dll)
    @{head = (& git -C $root rev-parse HEAD); scriptHash = (Get-FileHash $PSCommandPath).Hash
      installed = (Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt')); hashes = $hashes
      mission = $mission; missionHash = (Get-FileHash (Join-Path $mission 'mission.sqm')).Hash; environment = $environment
      originalActors = @('SoldierWB', 'Truck5t'); forceRefresh = [bool]$ForceRefresh; disabled = [bool]$Disabled; settleSeconds = 15
      scope = 'One original night fixture; fixed world/camera, sampled exact CPU pose witnesses; no retained/sky coverage or performance acceptance'
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
    $p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ArgumentList @(
        '--render=wgpu', '--window', '--dev', '--width', '1280', '--height', '720', '--vd', '2000',
        '--harness', [string]$port, '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
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
        @{utc = [DateTime]::UtcNow.ToString('o'); command = $command; reply = $reply} | ConvertTo-Json -Depth 8 -Compress | Add-Content (Join-Path $out 'harness.jsonl')
        if (!$reply.ok) { throw $line }; return $reply
    }
    function RequireResult($code, $expected) {
        $reply = Send @{cmd = 'eval'; code = $code}
        if ($reply.result.Trim('"') -ne $expected) { throw "Unexpected $code result: $($reply.result)" }
    }
    Start-Sleep -Seconds 15
    RequireResult 'typeOf lightTruck' 'Truck5t'; RequireResult 'typeOf player' 'SoldierWB'
    $null = Send @{cmd = 'exec'; code = 'driver lightTruck setBehaviour "SAFE"; lightTruck action ["LIGHT ON",lightTruck]; player setCombatMode "BLUE"; player disableAI "MOVE"; player disableAI "TARGET"; player setPos [5092.6,4040,0]; player setDir 180; player switchMove "EffectStandStill"'}
    # Exact view override leaves the original player visible as a third-person caster.
    RequireResult 'triSetView [5086,19,4045,6.6,-1.3,-5]' 'OK'
    RequireResult 'triPerfCapture 16384' 'OK'
    Start-Sleep -Seconds 6
    RequireResult 'getMove player' 'EffectStandStill'
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'before.png')}
    $beforePosition = (Send @{cmd = 'eval'; code = 'getPos player'}).result
    $null = Send @{cmd = 'exec'; code = 'player switchMove "Stand"'}
    Start-Sleep -Seconds 6
    RequireResult 'getMove player' 'Stand'
    $afterPosition = (Send @{cmd = 'eval'; code = 'getPos player'}).result
    if ($beforePosition -ne $afterPosition) { throw 'NotExercised: original player world position changed' }
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'after.png')}
    $null = Send @{cmd = 'exec'; code = 'player switchMove "EffectStandStill"'}
    Start-Sleep -Seconds 4
    RequireResult 'getMove player' 'EffectStandStill'
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'return.png')}
    RequireResult 'alive player' 'true'
    RequireResult 'triPerfCapture 0' 'OK'
    $null = Send @{cmd = 'exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game did not exit cleanly' }
    $rows = @()
    foreach ($path in @($log, $stdout, $stderr)) {
        if (!(Test-Path -LiteralPath $path)) { continue }
        if (Select-String -LiteralPath $path -Pattern 'Validation Error|panicked at|DeviceLost|StartAutoTest could not boot' -Quiet) { throw "Renderer/mission failure in $path" }
        foreach ($match in @(Select-String -LiteralPath $path -Pattern '\[wgr\] local pose cache(?: event)?: ')) {
            $values = @{}
            foreach ($field in [regex]::Matches($match.Line, '(\w+)=([^\s]+)')) { $values[$field.Groups[1].Value] = $field.Groups[2].Value }
            $values.event = $match.Line -match 'local pose cache event:'; $values.raw = $match.Line; $values.source = $path; $values.lineNumber = $match.LineNumber
            $rows += [pscustomobject]$values
        }
    }
    $rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'pose-trace.json')
    $begins = @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture begin n=(\d+) dropped=(\d+)')
    $ends = @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture end')
    if ($begins.Count -ne 1 -or $ends.Count -ne 1) { throw 'Expected exactly one complete engine frame capture' }
    $count = [int]$begins[0].Matches[0].Groups[1].Value; $dropped = [int]$begins[0].Matches[0].Groups[2].Value
    if ($count -lt 1 -or $count -gt 16384 -or $dropped -ne 0) { throw 'Engine frame count/drop evidence invalid' }
    $culture = [Globalization.CultureInfo]::InvariantCulture; $raw = @(); $phases = @()
    foreach ($row in @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture offset=(\d+) ms=(.*)$')) {
        if ([int]$row.Matches[0].Groups[1].Value -ne $raw.Count) { throw 'Raw frame series has a gap' }
        foreach ($value in $row.Matches[0].Groups[2].Value.Split(',')) { $raw += [double]::Parse($value, $culture) }
    }
    if ($raw.Count -ne $count) { throw 'Raw frame series differs from engine count' }
    foreach ($row in @(Select-String -LiteralPath $log -Pattern '\[tri\] triPerfCapture phase=(\S+) avg_ms=([\d.]+) p95_ms=([\d.]+) max_ms=([\d.]+)')) {
        $m = $row.Matches[0]; $phases += [pscustomobject]@{phase = $m.Groups[1].Value; avgMs = [double]::Parse($m.Groups[2].Value, $culture); p95Ms = [double]::Parse($m.Groups[3].Value, $culture); maxMs = [double]::Parse($m.Groups[4].Value, $culture)}
    }
    $expectedPhases = @('setup', 'sim:step', 'drw:init', 'drw:prep', 'land:gnd', 'land:obj', 'drw:land', 'drw:obj', 'drw:post', 'hud', 'sound', 'swap')
    if ($phases.Count -ne $expectedPhases.Count) { throw 'Frame phase evidence incomplete' }
    foreach ($phase in $expectedPhases) { if (@($phases | Where-Object { $_.phase -eq $phase }).Count -ne 1) { throw "Missing/duplicate frame phase: $phase" } }
    $raw | ConvertTo-Json | Set-Content (Join-Path $out 'route-frame-ms.json')
    $phases | ConvertTo-Json | Set-Content (Join-Path $out 'frame-phases.json')
    $eligible = @($rows | Where-Object { $_.enabled -eq 'true' -and [uint64]$_.cpuCasters -gt 0 -and [uint64]$_.mesh -gt 0 })
    if (!$Disabled -and !$ForceRefresh -and !$eligible.Count) { throw 'NotExercised: no actual CPU skinned local-shadow caster sample' }
    $witness = $null
    if (!$ForceRefresh -and !$Disabled) {
        for ($i = 0; $i -lt $eligible.Count -and !$witness; ++$i) {
            for ($j = $i + 1; $j -lt $eligible.Count -and !$witness; ++$j) {
                $a = $eligible[$i]; $b = $eligible[$j]
                if ($a.source -ne $b.source -or !$b.event -or [uint64]$b.sourcePlan -le [uint64]$a.sourcePlan) { continue }
                if ($a.mesh -ne $b.mesh -or $a.palette -ne $b.palette -or $a.worldContentKey -ne $b.worldContentKey -or
                    $a.indexBegin -ne $b.indexBegin -or $a.indexCount -ne $b.indexCount -or $a.instanceEpoch -ne $b.instanceEpoch -or
                    [uint64]$a.lightDirtyMask -ne 0 -or [uint64]$b.lightDirtyMask -ne 0 -or
                    [uint64]$a.revision -eq 0 -or [uint64]$b.revision -eq 0 -or $a.revision -eq $b.revision -or
                    [uint64]$b.changedCasters -eq 0 -or [uint64]$b.poseRefreshMask -eq 0 -or [uint64]$b.rendered -eq 0) { continue }
                $witness = @($a, $b)
            }
        }
        if (!$witness) { throw 'NotExercised: no same mesh/range/slot/world/epoch with changed pose revision, clean light and actual refresh' }
    } else {
        # OFF/force controls intentionally do not observe palette witnesses. They can
        # prove compatibility/capture completion only, never positive pose correctness.
        $expectedEnabled = if ($Disabled) { 'false' } else { 'true' }
        if (!@($rows | Where-Object { $_.enabled -eq $expectedEnabled -and [uint64]$_.rendered -gt 0 -and (!$ForceRefresh -or [uint64]$_.cached -eq 0) }).Count) {
            throw 'NotExercised: control did not trace actual local-plan rendering'
        }
        if ($ForceRefresh -and @($rows | Where-Object { [uint64]$_.cached -gt 0 }).Count) { throw 'Force-refresh control cached local views' }
        if ($Disabled -and @($rows | Where-Object { $_.enabled -eq 'true' }).Count) { throw 'Disabled control activated pose algorithm' }
    }
    $finalHashes = @(Get-FileHash $exe, $dll)
    if (($hashes.Hash -join ',') -ne ($finalHashes.Hash -join ',')) { throw 'Installed binaries changed during fixture' }
    @{passed = $true; controlOnly = [bool]($ForceRefresh -or $Disabled); disabled = [bool]$Disabled; forceRefresh = [bool]$ForceRefresh; frameCount = $count; droppedFrames = $dropped; sampledMutationWitness = $witness
      beforePosition = $beforePosition; afterPosition = $afterPosition; traceRows = $rows.Count
      visualJudgement = 'Screenshots retained; automated witness is CPU local caster scope only'
      timingScope = 'Narrow original fixture cost screening; no performance acceptance or broad workload parity'
    } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'result.json')
    $completed = $true; Write-Host "Bounded local pose fixture completed: $out"
} finally {
    if ($client) { $client.Dispose() }
    if ($p -and !$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(10000) } # own PID only
    foreach ($name in $changedNames) {
        if ($null -eq $savedEnvironment[$name]) { Remove-Item ('Env:' + $name) -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    }
    if (!$completed) { @{passed = $false; scope = 'Aborted or NotExercised; no positive pose-cache acceptance'} | ConvertTo-Json | Set-Content (Join-Path $out 'failed.json') }
}
