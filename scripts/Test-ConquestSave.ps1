<#
.SYNOPSIS
    Conquest save/load round trip: a saved war must load as that war, and go on.

.DESCRIPTION
    Runs the worktree's dist build (nothing is deployed) on the Everon war at 4x speed, saves it
    mid-war (triSaveGame), lets it run on, loads the save (triLoadGame) and checks:
      * the referee is active after the load and says "the saved war goes on";
      * objectives, pools (within income since the save), scores, squads and losses read as at the save;
      * no second "the war begins" (no fresh squads spawned on top of the loaded ones);
      * the loaded war runs: its status changes over the next minute and Conquest never stops.
    Writes build/conquest-runs/save-<time>/{engine.log,states.txt,*.png}. Exit code 1 on a failed check.
    Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
#>
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Bin = '',
    [int]$WarSeconds = 120,
    [int]$AfterSaveSeconds = 60
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $shellExe = Join-Path $PSHOME 'powershell.exe'
    $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe.Replace('\', '/'), '-NoProfile', '-File', $PSCommandPath,
        '-GameDir', $GameDir, '-WarSeconds', "$WarSeconds", '-AfterSaveSeconds', "$AfterSaveSeconds")
    if ($Bin) { $argv += @('-Bin', $Bin) }
    $env:LOCK_OWNER = 'conquest save/load test'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; exit $LASTEXITCODE }
    finally { Remove-Item Env:LOCK_OWNER }
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) { throw "Missing build: $exe" }
$out = Join-Path $root ('build/conquest-runs/save-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $out | Out-Null
$env:POSEIDON_USER_DIR = Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=1;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=60;'))
$env:POSEIDON_RNG_SEED = '5'
$mission = Join-Path $root 'content/default-packs/conquest/missions/conquest-everon.Eden'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
$log = Join-Path $out 'engine.log'
$gameArgs = @('-C', ('"' + $GameDir + '"'), '--render=wgpu', '--window', '--no-dev', '--width', '1280', '--height', '720',
    '--harness', "$port", '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
$p = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $Bin -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
$null = $p.Handle
$failures = @()
function Check($ok, $what) {
    if ($ok) { Write-Output "PASS $what" } else { Write-Output "FAIL $what"; $script:failures += $what }
}
# "Morton=WEST; ... | WEST pool 41 score 500 live 36 lost 0 bought 2 | ... | groups 10 | tick ..."
function Parse($state) {
    $o = @{ owners = ($state -split '\|')[0].Trim(); groups = -1; sides = @{} }
    if ($state -match 'groups (\d+)') { $o.groups = [int]$Matches[1] }
    foreach ($m in [regex]::Matches($state, '(\w+) pool (\d+) score (\d+) live (\d+) lost (\d+) bought (\d+)')) {
        $o.sides[$m.Groups[1].Value] = @{ pool = [int]$m.Groups[2].Value; score = [int]$m.Groups[3].Value
            live = [int]$m.Groups[4].Value; lost = [int]$m.Groups[5].Value; bought = [int]$m.Groups[6].Value }
    }
    return $o
}
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
    $reader = [IO.StreamReader]::new($stream)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do { $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $r = $line | ConvertFrom-Json } while ($null -eq $r.ok)
        if (!$r.ok) { throw $line }
        return $r
    }
    function Eval($code) { return "$((Send @{cmd = 'eval'; code = $code }).result)".Trim('"') }
    function Shot($name) { $null = Send @{cmd = 'screenshot'; path = (Join-Path $out ($name + '.png')) } }

    $deadline = [DateTime]::UtcNow.AddSeconds(180)
    while ((Eval 'conquestState') -match 'inactive') {
        if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Conquest never became active' }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 4
    $null = Eval 'conquestCommanderView true'
    $null = Eval 'conquestCommanderView false'
    $null = Eval 'setAccTime 4'
    Start-Sleep -Seconds $WarSeconds

    $null = Eval 'setAccTime 1'
    $saved = Eval 'conquestState'
    $savedTime = [double](Eval 'time')
    $result = Eval 'triSaveGame "conquest-save-test"'
    Check ($result -eq 'OK') "save written ($result)"
    Shot 'saved'
    $null = Eval 'setAccTime 4'
    Start-Sleep -Seconds $AfterSaveSeconds
    $null = Eval 'setAccTime 1'
    $before = Eval 'conquestState'

    $result = Eval 'triLoadGame "conquest-save-test"'
    Check ($result -eq 'OK') "save loaded ($result)"
    Start-Sleep -Seconds 5
    $loaded = Eval 'conquestState'
    $loadedTime = [double](Eval 'time')
    Shot 'loaded'
    $null = Eval 'setAccTime 4'
    Start-Sleep -Seconds $AfterSaveSeconds
    $null = Eval 'setAccTime 1'
    $after = Eval 'conquestState'
    Shot 'after'
    [IO.File]::WriteAllLines((Join-Path $out 'states.txt'), @(
        "saved  (t=$savedTime): $saved", "before load: $before", "loaded (t=$loadedTime): $loaded", "after: $after"))

    # Compare with the status the referee logs at the moment it restores the war: by the time a
    # harness call reads conquestState, the war has moved on (a purchase, a man lost).
    $restored = (Select-String -Path $log -Pattern 'Conquest: the saved war goes on' -Context 0, 3 |
        Select-Object -First 1).Context.PostContext | Where-Object { $_ -match 'Conquest status: (.*)$' } | Select-Object -First 1
    $restoredState = if ($restored -match 'Conquest status: (.*)$') { $Matches[1] } else { 'missing' }
    Add-Content (Join-Path $out 'states.txt') "restored: $restoredState"
    $s = Parse $saved; $l = Parse $restoredState; $a = Parse $after
    Check ($loaded -notmatch 'inactive') 'Conquest active after the load'
    Check ([Math]::Abs($loadedTime - $savedTime) -lt 30) "game time back at the save ($savedTime -> $loadedTime)"
    Check ($l.owners -eq $s.owners) "objectives as saved ($($l.owners))"
    Check ($l.groups -eq $s.groups) "squads as saved ($($s.groups) -> $($l.groups))"
    foreach ($side in $s.sides.Keys) {
        $x = $s.sides[$side]; $y = $l.sides[$side]
        Check ($null -ne $y) "$side present after the load"
        if ($null -eq $y) { continue }
        Check ($y.lost -eq $x.lost -and $y.bought -eq $x.bought) "$side losses and purchases as saved ($($x.lost)/$($x.bought) -> $($y.lost)/$($y.bought))"
        Check ([Math]::Abs($y.score - $x.score) -le 5) "$side score as saved ($($x.score) -> $($y.score))"
        Check ($y.pool -ge $x.pool - 1 -and $y.pool -le $x.pool + 100) "$side pool as saved ($($x.pool) -> $($y.pool))"
        Check ([Math]::Abs($y.live - $x.live) -le 2) "$side soldiers as saved ($($x.live) -> $($y.live))"
    }
    Check ($after -ne $loaded -and $after -notmatch 'inactive') 'the loaded war goes on'
    $text = Get-Content $log -Raw
    Check (([regex]::Matches($text, 'Conquest: the war begins')).Count -eq 1) 'no second "the war begins" after the load'
    Check ($text -match 'Conquest: the saved war goes on') 'referee continued the saved war'
    Check ($text -notmatch 'Conquest stops') 'Conquest never stopped'

    $null = Send @{cmd = 'exit' }
    if (!$p.WaitForExit(30000)) { throw 'Exit timed out' }
} finally {
    if ($client) { $client.Dispose() }
    if (!$p.HasExited) { Stop-Process -Id $p.Id; $p.WaitForExit() }
}
Write-Output "Conquest save test: $out"
if ($failures.Count) { Write-Output "$($failures.Count) check(s) failed"; exit 1 }
Write-Output 'all checks passed'
