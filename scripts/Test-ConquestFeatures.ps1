<#
.SYNOPSIS
    Conquest feature check: setup screen, death carry-on, HQ orders, command posts, shroud, mortars,
    the follow camera at ground level. Captures go to build/conquest-runs/features-<time>/.

.DESCRIPTION
    Two runs of the worktree's dist build (nothing is deployed) on the Everon war:
      A  WEST, as a soldier, no commander view: HQ orders the player's squad; killing the player twice
         must carry them on into other soldiers, never end the mission.
      B  EAST, in command, shroud: the command view with posts and symbols, a mortar barrage, the
         camera following a squad leader down to the ground; the possessed soldier's death returns
         the player to command.
    Exit code 1 on a failed check. Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
#>
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Bin = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $shellExe = Join-Path $PSHOME 'powershell.exe'
    $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe.Replace('\', '/'), '-NoProfile', '-File', $PSCommandPath,
        '-GameDir', $GameDir)
    if ($Bin) { $argv += @('-Bin', $Bin) }
    $env:LOCK_OWNER = 'conquest feature test'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; exit $LASTEXITCODE }
    finally { Remove-Item Env:LOCK_OWNER }
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) { throw "Missing build: $exe" }
$base = Join-Path $root ('build/conquest-runs/features-' + (Get-Date -Format yyyyMMdd-HHmmss))
$mission = Join-Path $root 'content/default-packs/conquest/missions/conquest-everon.Eden'
$script:failures = @()
function Check($ok, $what) {
    if ($ok) { Write-Output "PASS $what" } else { Write-Output "FAIL $what"; $script:failures += $what }
}

function Invoke-Run($name, [scriptblock]$body) {
    $out = Join-Path $base $name
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:POSEIDON_USER_DIR = Join-Path $out 'user'
    New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
    [IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=1;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=60;'))
    $env:POSEIDON_RNG_SEED = '9'
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
        $stream = $client.GetStream(); $stream.ReadTimeout = 120000
        $script:reader = [IO.StreamReader]::new($stream)
        $script:writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $script:writer.AutoFlush = $true
        $script:out = $out
        $script:log = $log
        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        while ((Eval 'conquestState') -match 'inactive') {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Conquest never became active' }
            Start-Sleep -Seconds 2
        }
        Start-Sleep -Seconds 3
        & $body
        Check (!$p.HasExited) "$name : the game is still running"
        $null = Send @{cmd = 'exit' }
        $null = $p.WaitForExit(30000)
    } finally {
        if ($client) { $client.Dispose() }
        if (!$p.HasExited) { Stop-Process -Id $p.Id; $p.WaitForExit() }
    }
}
function Send($command) {
    $script:writer.WriteLine(($command | ConvertTo-Json -Compress))
    do { $line = $script:reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $r = $line | ConvertFrom-Json } while ($null -eq $r.ok)
    if (!$r.ok) { throw $line }
    return $r
}
function Eval($code) { return "$((Send @{cmd = 'eval'; code = $code }).result)".Trim('"') }
function Shot($name) { $null = Send @{cmd = 'screenshot'; path = (Join-Path $script:out ($name + '.png')) } }
function LogCount($pattern) { return ([regex]::Matches((Get-Content $script:log -Raw), $pattern)).Count }

Invoke-Run 'A-soldier' {
    Shot 'setup-screen'
    Check ((Eval 'conquestStart ["WEST", "soldier", "contacts", "noview"]') -eq 'true') 'A: the war starts from conquestStart'
    Start-Sleep -Seconds 12
    Shot 'soldier-hq-order'
    Check ((LogCount 'HQ to the player squad') -ge 1) 'A: HQ gave the player''s squad an order'
    Check ((LogCount 'Conquest: the war begins') -eq 1) 'A: the war began once'
    foreach ($k in 1..2) {
        $null = Eval 'player setDammage 1'
        Start-Sleep -Seconds 5
        Shot "after-death-$k"
        Check ((Eval 'alive player') -eq 'true') "A: after death $k the player is a living soldier"
        Check ((LogCount 'the player was killed and carries on') -ge $k) "A: death $k carried the player on"
    }
    Start-Sleep -Seconds 6
    Check ((Eval 'conquestState') -notmatch 'inactive') 'A: the war still runs after two deaths'
}

Invoke-Run 'B-commander' {
    Check ((Eval 'conquestStart ["EAST", "commander", "shroud"]') -eq 'true') 'B: the war starts in command, shroud'
    Start-Sleep -Seconds 8
    Check ((LogCount 'commander view on') -ge 1) 'B: the command view opened at the start'
    $null = Eval 'conquestCamera [4772, 10700, 700, 55, 180]'
    Start-Sleep -Seconds 2
    Shot 'command-shroud-airfield'
    $null = Eval 'conquestCamera [5000, 7500, 3000, 70, 180]'
    Start-Sleep -Seconds 2
    Shot 'command-shroud-overview'
    # The command post at the airfield, close.
    $null = Eval 'conquestCamera [4772, 10923, 90, 35, 200]'
    Start-Sleep -Seconds 2
    Shot 'command-post-airfield'
    # Mortars on open ground near the airfield, watched from a safe distance.
    $result = Eval 'conquestBarrage [4700, 10450]'
    Check ($result -eq 'true') "B: the barrage was ordered ($result)"
    $null = Eval 'conquestCamera [4700, 10450, 350, 40, 180]'
    Start-Sleep -Seconds 10
    Shot 'barrage'
    Start-Sleep -Seconds 4
    Shot 'barrage-2'
    Check ((Eval 'conquestBarrage [4700, 10450]') -match 'reloading') 'B: the mortars reload after a barrage'
    # Follow an EAST squad leader, then all the way down to the ground.
    Check ((Eval 'conquestCamera [4, 25, 30]') -eq 'true') 'B: following squad 4'
    Start-Sleep -Seconds 3
    Shot 'follow-25m'
    $null = Eval 'conquestCamera [4, 3, 3]'
    Start-Sleep -Seconds 3
    Shot 'follow-ground-level'
    # Into a soldier, killed: back in command.
    $took = Eval 'conquestTakeControl 5'
    Check ($took -eq 'true') "B: took control of squad 5 ($took)"
    Start-Sleep -Seconds 3
    $null = Eval 'player setDammage 1'
    Start-Sleep -Seconds 5
    Shot 'back-in-command'
    Check ((LogCount 'the soldier you controlled was killed; back in command') -ge 1) 'B: the death returned the player to command'
    Check ((Eval 'conquestState') -notmatch 'inactive') 'B: the war still runs'
}

Invoke-Run 'C-barrage' {
    # Mortars watched with the default fog of war, on open ground west of the airfield's runway.
    Check ((Eval 'conquestStart ["EAST", "commander", "contacts"]') -eq 'true') 'C: the war starts in command'
    Start-Sleep -Seconds 6
    $null = Eval 'conquestCamera [4600, 10650, 260, 38, 90]'
    Check ((Eval 'conquestBarrage [4600, 10650]') -eq 'true') 'C: the barrage was ordered'
    Start-Sleep -Seconds 6
    foreach ($t in 1..8) {
        Start-Sleep -Milliseconds 1300
        Shot "rounds-$t"
    }
}

Write-Output "Conquest feature test: $base"
if ($script:failures.Count) { Write-Output "$($script:failures.Count) check(s) failed"; exit 1 }
Write-Output 'all checks passed'
