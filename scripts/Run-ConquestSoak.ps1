<#
.SYNOPSIS
    Conquest Slice 0 soak: runs the NPC-only Everon war and records how it goes.

.DESCRIPTION
    Runs the worktree's dist build against the game data (nothing is deployed) on
    dev-missions/conquest-everon.Eden, speeds time up, and every PollSeconds of real time asks the
    referee for `conquestState` and the game clock through the harness. Writes:
      * state.jsonl  - one line per poll: real seconds, game seconds, the referee's status line;
      * map-*.png    - the in-game map (objectives in their owner's colour) at intervals;
      * engine.log   - the engine log, with every "Conquest:" event and "Conquest status:" line.
    The war's checks (captures per hour, snowball, end, cap, stalls) are read from these files.

    Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
#>
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Bin = '',
    [double]$Minutes = 20,
    [double]$AccTime = 4,
    [int]$PollSeconds = 30,
    [int]$ShotEveryPolls = 10,
    [int]$Seed = 0,
    [switch]$CommanderTest,
    [switch]$Visible
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $shellExe = Join-Path $PSHOME 'powershell.exe'
    $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe.Replace('\', '/'), '-NoProfile', '-File', $PSCommandPath,
        '-GameDir', $GameDir, '-Minutes', "$Minutes", '-AccTime', "$AccTime", '-PollSeconds', "$PollSeconds",
        '-ShotEveryPolls', "$ShotEveryPolls", '-Seed', "$Seed")
    if ($Bin) { $argv += @('-Bin', $Bin) }
    if ($Visible) { $argv += '-Visible' }
    if ($CommanderTest) { $argv += '-CommanderTest' }
    $env:LOCK_OWNER = 'conquest soak'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) { throw "Conquest soak exited $LASTEXITCODE" } }
    finally { Remove-Item Env:LOCK_OWNER }
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
$out = Join-Path $root ('build/conquest-runs/' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $out | Out-Null
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) { throw "Missing build: $exe" }
$env:POSEIDON_USER_DIR = Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=1;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=60;'))
if ($Seed) { $env:POSEIDON_RNG_SEED = "$Seed" }
$mission = Join-Path $root 'dev-missions/conquest-everon.Eden'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
$log = Join-Path $out 'engine.log'
$gameArgs = @('-C', ('"' + $GameDir + '"'), '--render=wgpu', '--window', '--no-dev', '--width', '1280', '--height', '720',
    '--harness', "$port", '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
$style = 'Hidden'
if ($Visible) { $style = 'Normal' }
$p = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $Bin -PassThru -WindowStyle $style `
    -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
$null = $p.Handle
$jsonl = Join-Path $out 'state.jsonl'
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
    function Sqf($code) { $null = Send @{cmd = 'exec'; code = $code } }
    function Eval($code) { return (Send @{cmd = 'eval'; code = $code }).result }
    function Shot($name) {
        Sqf 'forceMap true; mapAnimAdd [0, 0.85, [3900, 7400, 0]]; mapAnimCommit'
        Start-Sleep -Seconds 2
        $null = Send @{cmd = 'screenshot'; path = (Join-Path $out ($name + '.png')) }
        Start-Sleep -Seconds 1
        Sqf 'forceMap false'
    }

    # Wait for the referee to start the war (the mission is loaded and simulating).
    $deadline = [DateTime]::UtcNow.AddSeconds(180)
    while ((Eval 'conquestState') -match 'inactive') {
        if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Conquest never became active' }
        Start-Sleep -Seconds 2
    }
    # A mission may start the player as a squad leader (startAs = "soldier"). Picture the soldier's
    # HUD, then hand the squad back to the AI: the soak watches an NPC-only war.
    Start-Sleep -Seconds 4
    $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'soldier-hud.png') }
    $null = Eval 'conquestCommanderView true'
    $null = Eval 'conquestCommanderView false'
    Sqf "setAccTime $AccTime"
    if ($CommanderTest) {
        # The player commands WEST: select the squad nearest Morton, then send it to Montignac.
        # The referee logs "commander: ..." and the squad's map label gains "[cmd]".
        Start-Sleep -Seconds 5
        $picked = Eval 'conquestMapClick [[5134, 3955, 0], false, false]'
        $ordered = Eval 'conquestMapClick [[4935, 6994, 0], false, false]'
        Write-Output "commander test: select=$picked order=$ordered"
        # The top-down commander view (Ctrl+M), and a purchase through the same path its HUD uses.
        $view = Eval 'conquestCommanderView true'
        Start-Sleep -Seconds 3
        $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'commander-0.png') }
        $bought = Eval 'conquestBuy ["Morton", "Recon"]'
        Write-Output "commander view: open=$view buy=$bought"
        Start-Sleep -Seconds 20
        $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'commander-1.png') }
        Start-Sleep -Seconds 1
        # Take over a squad leader (the first WEST squad), look through his eyes, then back to command.
        $took = Eval 'conquestTakeControl 1'
        Start-Sleep -Seconds 4
        $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'commander-2-soldier.png') }
        $back = Eval 'conquestCommanderView true'
        Start-Sleep -Seconds 3
        $null = Send @{cmd = 'screenshot'; path = (Join-Path $out 'commander-3-back.png') }
        Write-Output "take control: $took, back in command: $back"
        $null = Eval 'conquestCommanderView false'
    }
    $start = [DateTime]::UtcNow
    $poll = 0
    while (([DateTime]::UtcNow - $start).TotalMinutes -lt $Minutes) {
        if ($p.HasExited) { throw 'Game exited during the soak' }
        $state = Eval 'conquestState'
        $game = Eval 'time'
        (@{ real = [Math]::Round(([DateTime]::UtcNow - $start).TotalSeconds); game = $game; state = $state } |
            ConvertTo-Json -Compress) | Add-Content $jsonl
        Write-Output ("[{0,5:N0}s real, {1} game] {2}" -f ([DateTime]::UtcNow - $start).TotalSeconds, $game, $state)
        if ($poll % $ShotEveryPolls -eq 0) { Shot ('map-{0:D3}' -f $poll) }
        if ($state -match 'wins|DEFEATED' -and $state -notmatch 'inactive') {
            if (((Get-Content $jsonl) | Select-String 'DEFEATED').Count -ge 2) { break }
        }
        $poll++
        Start-Sleep -Seconds $PollSeconds
    }
    Shot 'map-final'
    $null = Send @{cmd = 'exit' }
    if (!$p.WaitForExit(30000)) { throw 'Exit timed out' }
    Write-Output "Conquest soak complete: $out"
} finally {
    if ($client) { $client.Dispose() }
    if (!$p.HasExited) { Stop-Process -Id $p.Id; $p.WaitForExit() }
}
