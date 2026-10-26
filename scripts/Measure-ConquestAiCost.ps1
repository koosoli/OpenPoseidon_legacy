<#
.SYNOPSIS
    What a Conquest war costs per frame at a given number of living soldiers.

.DESCRIPTION
    For each total in -Totals (both sides together), runs the worktree's dist build (nothing is
    deployed) on a copy of the Everon war whose cap is total/2 per side and whose supplies are
    effectively endless, so the AI fills the cap and refills it as soldiers die. Once the war holds
    at least -FillShare of the target alive, it samples the engine's frame profiler (`triPerfStats`:
    the fixed-step simulation loop "sim:step", where AI and vehicles run, and the whole frame) and the
    referee's own tick (`conquestState`) every -SampleSeconds for -Samples samples, at normal speed.
    The player's squad goes back to the AI first, so every soldier is an NPC.

    Writes build/conquest-cost/<time>/<total>/{samples.jsonl,engine.log} and a summary.md table.
    Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
#>
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Bin = '',
    [string]$Totals = '80,160,240',
    [double]$FillShare = 0.85,
    [int]$FillMinutes = 8,
    [int]$Samples = 12,
    [int]$SampleSeconds = 10,
    [int]$Seed = 7
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $shellExe = Join-Path $PSHOME 'powershell.exe'
    $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe.Replace('\', '/'), '-NoProfile', '-File', $PSCommandPath,
        '-GameDir', $GameDir, '-Totals', $Totals, '-FillShare', "$FillShare", '-FillMinutes', "$FillMinutes",
        '-Samples', "$Samples", '-SampleSeconds', "$SampleSeconds", '-Seed', "$Seed")
    if ($Bin) { $argv += @('-Bin', $Bin) }
    $env:LOCK_OWNER = 'conquest AI cost'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) { throw "Conquest cost run exited $LASTEXITCODE" } }
    finally { Remove-Item Env:LOCK_OWNER }
    return
}
$targets = @($Totals.Split(',') | ForEach-Object { [int]$_.Trim() })
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) { throw "Missing build: $exe" }
$base = Join-Path $root ('build/conquest-cost/' + (Get-Date -Format yyyyMMdd-HHmmss))
$source = Join-Path $root 'content/default-packs/conquest/missions/conquest-everon.Eden'
$commit = (& git -C $root rev-parse --short HEAD)
$rows = @()

function Field($text, $name) {
    if ($text -match ([regex]::Escape($name) + '=([\d.]+)/([\d.]+)')) { return @([double]$Matches[1], [double]$Matches[2]) }
    return @([double]::NaN, [double]::NaN)
}
function Live($state) {
    $n = 0
    foreach ($m in [regex]::Matches($state, 'live (\d+)')) { $n += [int]$m.Groups[1].Value }
    return $n
}

foreach ($total in $targets) {
    $out = Join-Path $base "$total"
    New-Item -ItemType Directory -Force $out | Out-Null
    # The mission, with the cap at half the total per side and supplies that never run out.
    $mission = Join-Path $out 'conquest-everon.Eden'
    Copy-Item -LiteralPath $source -Destination $out -Recurse
    $ext = Join-Path $mission 'description.ext'
    $text = [IO.File]::ReadAllText($ext)
    $text = $text -replace '(?m)^(\s*)cap\s*=\s*\d+;', ('$1cap = ' + [int]($total / 2) + ';')
    $text = $text -replace '(?m)^(\s*)startPool\s*=\s*\d+;', '$1startPool = 1000000;'
    [IO.File]::WriteAllText($ext, $text)

    $env:POSEIDON_USER_DIR = Join-Path $out 'user'
    New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
    [IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=1;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=0;'))
    $env:POSEIDON_RNG_SEED = "$Seed"
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $log = Join-Path $out 'engine.log'
    $gameArgs = @('-C', ('"' + $GameDir + '"'), '--render=wgpu', '--window', '--no-dev', '--width', '1280', '--height', '720',
        '--harness', "$port", '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
    $p = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $Bin -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
    $null = $p.Handle
    $client = $null
    try {
        $client = [Net.Sockets.TcpClient]::new()
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        while (!$client.Connected) {
            try { $client.Connect('127.0.0.1', $port) } catch {
                if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw }
                Start-Sleep -Milliseconds 250
            }
        }
        $stream = $client.GetStream(); $stream.ReadTimeout = 60000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
        function Send($command) {
            $writer.WriteLine(($command | ConvertTo-Json -Compress))
            do { $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $r = $line | ConvertFrom-Json } while ($null -eq $r.ok)
            if (!$r.ok) { throw $line }
            return $r
        }
        function Eval($code) { return (Send @{cmd = 'eval'; code = $code }).result }

        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        while ((Eval 'conquestState') -match 'inactive') {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Conquest never became active' }
            Start-Sleep -Seconds 2
        }
        # The mission opens on its setup screen: start the war as WEST, leading a squad.
        $null = Eval 'conquestStart ["WEST", "soldier"]'
        # The player's squad goes back to the AI: every soldier measured is an NPC.
        Start-Sleep -Seconds 4
        $null = Eval 'conquestCommanderView true'
        $null = Eval 'conquestCommanderView false'
        $null = Eval 'setAccTime 1'

        $fillTarget = [int]($total * $FillShare)
        $deadline = [DateTime]::UtcNow.AddMinutes($FillMinutes)
        do {
            Start-Sleep -Seconds 5
            if ($p.HasExited) { throw 'Game exited while the war filled' }
            $state = Eval 'conquestState'
            $live = Live $state
            Write-Output ("[$total] filling: $live of $fillTarget alive")
        } while ($live -lt $fillTarget -and [DateTime]::UtcNow -lt $deadline)
        $filled = $live -ge $fillTarget

        $jsonl = Join-Path $out 'samples.jsonl'
        $sim = @(); $sim95 = @(); $frame = @(); $frame95 = @(); $fps = @(); $lives = @(); $tick = @()
        for ($i = 0; $i -lt $Samples; $i++) {
            Start-Sleep -Seconds $SampleSeconds
            if ($p.HasExited) { throw 'Game exited while sampling' }
            $perf = Eval 'triPerfStats 0'
            $state = Eval 'conquestState'
            $live = Live $state
            (@{ i = $i; live = $live; perf = $perf; state = $state } | ConvertTo-Json -Compress) | Add-Content $jsonl
            $s = Field $perf 'sim:step'; $f = Field $perf 'frame'
            if ($perf -match 'fps=([\d.]+)') { $fps += [double]$Matches[1] }
            if ($state -match 'tick ([\d.]+) ms') { $tick += [double]$Matches[1] }
            $sim += $s[0]; $sim95 += $s[1]; $frame += $f[0]; $frame95 += $f[1]; $lives += $live
            Write-Output ("[$total] sample $i : live $live sim:step {0}/{1} ms frame {2}/{3} ms" -f $s[0], $s[1], $f[0], $f[1])
        }
        $mean = { param($a) if ($a.Count) { [Math]::Round((($a | Measure-Object -Average).Average), 2) } else { 'n/a' } }
        $rows += [pscustomobject]@{
            Target = $total; Filled = $filled; Live = (& $mean $lives); SimAvg = (& $mean $sim); SimP95 = (& $mean $sim95)
            FrameAvg = (& $mean $frame); FrameP95 = (& $mean $frame95); Fps = (& $mean $fps); RefereeTick = (& $mean $tick)
        }
        $null = Send @{cmd = 'exit' }
        if (!$p.WaitForExit(30000)) { throw 'Exit timed out' }
    } finally {
        if ($client) { $client.Dispose() }
        if (!$p.HasExited) { Stop-Process -Id $p.Id; $p.WaitForExit() }
    }
}

$md = @("# Conquest AI cost ($commit, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))", '',
    "Everon war, normal speed, 1280x720 windowed, quality preset 1, no vsync or frame cap. Means of $Samples samples of",
    "triPerfStats (a rolling window of the last frames) taken every $SampleSeconds s once the war held $([int]($FillShare*100))% of the target alive.", '',
    '| Target alive | Filled | Mean alive | sim:step avg ms | sim:step p95 ms | frame avg ms | frame p95 ms | fps | referee tick ms |',
    '|---:|---|---:|---:|---:|---:|---:|---:|---:|')
foreach ($r in $rows) {
    $md += "| $($r.Target) | $($r.Filled) | $($r.Live) | $($r.SimAvg) | $($r.SimP95) | $($r.FrameAvg) | $($r.FrameP95) | $($r.Fps) | $($r.RefereeTick) |"
}
[IO.File]::WriteAllLines((Join-Path $base 'summary.md'), $md)
$md | Write-Output
Write-Output "Conquest AI cost complete: $base"
