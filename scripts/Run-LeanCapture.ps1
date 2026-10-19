<#
.SYNOPSIS
    Corner-peek (lean) evidence run: real Q/E key presses, captures and numbers.

.DESCRIPTION
    Runs the worktree's dist build against the game data (nothing is deployed), loads
    tests/perf/missions/lean_corner.Intro, drops a barracks from the game's own config next to
    the player, finds its south-west corner with the fire-geometry probe, and then:
      * stands the player behind the corner, facing north, the target hidden behind the wall;
      * presses Q / E through the harness key injection (the real input path, not a script
        override), first person, third person (an orbit camera looking at him) and crouched;
      * fires once upright (into the wall) and once leaned left (past the corner) with the
        ballistics trails on;
      * hugs the west wall and leans right into it, to show the wall stopping the lean.
    Every step logs `lean_state` (lean, eye, muzzle, weapon direction in world space) to
    lean.jsonl, so the eye and muzzle displacement are measured, not eyeballed.

    Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
#>
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Bin = '',
    [switch]$Visible
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $shellExe = Join-Path $PSHOME 'powershell.exe'
    $argv = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe.Replace('\', '/'), '-NoProfile', '-File', $PSCommandPath, '-GameDir', $GameDir)
    if ($Bin) { $argv += @('-Bin', $Bin) }
    if ($Visible) { $argv += '-Visible' }
    $env:LOCK_OWNER = 'corner-peek lean capture'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) { throw "Lean capture exited $LASTEXITCODE" } }
    finally { Remove-Item Env:LOCK_OWNER }
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
$out = Join-Path $root ('build/lean-runs/' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $out | Out-Null
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) { throw "Missing build: $exe" }
$env:POSEIDON_USER_DIR = Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=3;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=60;'))
$mission = Join-Path $root 'tests/perf/missions/lean_corner.Intro'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
$log = Join-Path $out 'engine.log'
$gameArgs = @('-C', ('"' + $GameDir + '"'), '--render=wgpu', '--window', '--no-dev', '--width', '1280', '--height', '720', '--harness', "$port", '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $log + '"'))
$style = 'Hidden'
if ($Visible) { $style = 'Normal' }
$p = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $Bin -PassThru -WindowStyle $style `
    -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
$null = $p.Handle
$jsonl = Join-Path $out 'lean.jsonl'
try {
    $client = [Net.Sockets.TcpClient]::new()
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (!$client.Connected) {
        try { $client.Connect('127.0.0.1', $port) } catch {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
    $stream = $client.GetStream(); $stream.ReadTimeout = 30000
    $reader = [IO.StreamReader]::new($stream)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do { $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $r = $line | ConvertFrom-Json } while ($null -eq $r.ok)
        if (!$r.ok) { throw $line }
        return $r
    }
    function Note($text) { Write-Output $text; Add-Content -LiteralPath (Join-Path $out 'scene.txt') -Value $text }
    function Sqf($code) { $null = Send @{cmd = 'exec'; code = $code } }
    function Eval($code) { return (Send @{cmd = 'eval'; code = $code }) }
    function State($label) {
        $s = Send @{cmd = 'lean_state' }
        (@{label = $label; state = $s } | ConvertTo-Json -Compress -Depth 6) | Add-Content $jsonl
        return $s
    }
    function Shot($name) { $null = Send @{cmd = 'screenshot'; path = (Join-Path $out ($name + '.png')) }; Start-Sleep -Milliseconds 400 }
    # SDL scancodes: Q = 20, E = 8.
    function Hold($sc) { $null = Send @{cmd = 'key'; sc = $sc; hold = $true } }
    function Release($sc) { $null = Send @{cmd = 'key_up'; sc = $sc } }
    function Leaned($name, $sc, $label) {
        Hold $sc; Start-Sleep -Milliseconds 900
        $null = State $label; Shot $name
        Release $sc; Start-Sleep -Milliseconds 700
    }
    # A wall is wherever a probe at any of a few heights meets fire geometry: glass has none,
    # so one height alone passes through every window.
    # Turn him with real mouse motion until the barrel points along $bearing (degrees from north),
    # closed loop on the measured weapon direction: the player's aim follows the cursor, so setDir
    # alone is undone a moment later.
    function Bearing($v) { return [Math]::Atan2([double]$v[0], [double]$v[2]) * 180 / [Math]::PI }
    function Elevation($v) { return [Math]::Asin([Math]::Max(-1.0, [Math]::Min(1.0, [double]$v[1]))) * 180 / [Math]::PI }
    # Steer the barrel to (bearing, elevation) in degrees with real mouse motion, closed loop on
    # the measured weapon direction, with signed gains learnt from each step.
    function TurnTo($bearing, $elevation = 0.0) {
        $gx = 4.0; $gy = -4.0
        for ($k = 0; $k -lt 14; $k++) {
            $st = Send @{cmd = 'lean_state' }
            $hb = Bearing $st.weaponDir; $he = Elevation $st.weaponDir
            $eb = (($bearing - $hb + 540) % 360) - 180; $ee = $elevation - $he
            if ([Math]::Abs($eb) -lt 0.15 -and [Math]::Abs($ee) -lt 0.15) { break }
            $dx = 0; $dy = 0
            if ([Math]::Abs($eb) -ge 0.15) { $dx = [int][Math]::Round($eb * $gx); if ($dx -eq 0) { $dx = [Math]::Sign($eb * $gx) } }
            if ([Math]::Abs($ee) -ge 0.15) { $dy = [int][Math]::Round($ee * $gy); if ($dy -eq 0) { $dy = [Math]::Sign($ee * $gy) } }
            $dx = [Math]::Max(-300, [Math]::Min(300, $dx)); $dy = [Math]::Max(-300, [Math]::Min(300, $dy))
            $null = Send @{cmd = 'mouse_motion'; dx = $dx; dy = $dy }
            Start-Sleep -Milliseconds 400
            $st2 = Send @{cmd = 'lean_state' }
            $mb = (((Bearing $st2.weaponDir) - $hb + 540) % 360) - 180; $me = (Elevation $st2.weaponDir) - $he
            if ($dx -ne 0 -and [Math]::Abs($mb) -gt 0.05) { $g = $dx / $mb; if ([Math]::Abs($g) -lt 40 -and [Math]::Abs($g) -gt 0.3) { $gx = $g } }
            if ($dy -ne 0 -and [Math]::Abs($me) -gt 0.05) { $g = $dy / $me; if ([Math]::Abs($g) -lt 40 -and [Math]::Abs($g) -gt 0.3) { $gy = $g } }
        }
    }
    function AimAtTarget() {
        $t = (Eval 'getPos leanTarget').result.Trim('[', ']').Split(',')
        $ty = $py + 1.0 # his chest
        for ($pass = 0; $pass -lt 2; $pass++) {
            $st = Send @{cmd = 'lean_state' }
            $ddx = [double]$t[0] - [double]$st.muzzle[0]; $ddz = [double]$t[1] - [double]$st.muzzle[2]
            TurnTo ([Math]::Atan2($ddx, $ddz) * 180 / [Math]::PI) ([Math]::Atan2($ty - [double]$st.muzzle[1], [Math]::Sqrt($ddx * $ddx + $ddz * $ddz)) * 180 / [Math]::PI)
        }
        $st = Send @{cmd = 'lean_state' }
        Note ("aim: barrel " + (Bearing $st.weaponDir) + " deg / " + (Elevation $st.weaponDir) + " deg up, target bearing from muzzle " + ([Math]::Atan2([double]$t[0] - [double]$st.muzzle[0], [double]$t[1] - [double]$st.muzzle[2]) * 180 / [Math]::PI) + " deg")
    }
    function Probe($from, $to) {
        foreach ($dy in @(-0.9, -0.4, 0.4, 1.0)) {
            $f = @($from[0], ($from[1] + $dy), $from[2]); $t = @($to[0], ($to[1] + $dy), $to[2])
            $r = Send @{cmd = 'stream_fire_probe'; from = $f; to = $t }
            if ((@($r.contacts) | Where-Object { $_ }).Count -gt 0) { return $true }
        }
        return $false
    }

    Start-Sleep -Seconds 15
    $s0 = State 'spawn'
    $px = [double]$s0.position[0]; $py = [double]$s0.position[1]; $pz = [double]$s0.position[2]
    # The wall: a barracks 12 m north of the player, square to the world axes.
    $cx = $px; $cz = $pz + 14
    Sqf ("leanWall = ""Barracks"" createVehicle [$cx,$cz,0]; leanWall setDir 0; leanWall setPos [$cx,$cz,0]")
    # Off the probe lines while the wall is measured (the probes see soldiers too).
    Sqf ("leanPlayer setPos [" + ($px + 40) + "," + ($pz - 30) + ",0]; leanTarget setPos [" + ($px + 44) + "," + ($pz - 30) + ",0]")
    Start-Sleep -Seconds 2
    $h = $py + 1.2
    # South face: shortest probe from the south that touches it, along x = centre.
    $lo = 0.0; $hi = 30.0
    if (!(Probe @($cx, $h, ($cz - 30)) @($cx, $h, $cz))) { throw 'No barracks fire geometry found at the centre line' }
    for ($i = 0; $i -lt 14; $i++) { $m = ($lo + $hi) / 2; if (Probe @($cx, $h, ($cz - 30)) @($cx, $h, ($cz - 30 + $m))) { $hi = $m } else { $lo = $m } }
    $z0 = $cz - 30 + $hi
    # The end walls: segments coming in along x from outside, 1 m inside the south face. (Along
    # the face itself the windows make "is there wall at this x" non-monotonic; an end wall met
    # from outside is not.)
    $zi = $z0 + 1.0
    $lo = 0.0; $hi = 30.0
    for ($i = 0; $i -lt 16; $i++) { $m = ($lo + $hi) / 2; if (Probe @(($cx - 30), $h, $zi) @(($cx - 30 + $m), $h, $zi)) { $hi = $m } else { $lo = $m } }
    $x0 = $cx - 30 + $hi
    $lo = 0.0; $hi = 30.0
    for ($i = 0; $i -lt 16; $i++) { $m = ($lo + $hi) / 2; if (Probe @(($cx + 30), $h, $zi) @(($cx + 30 - $m), $h, $zi)) { $hi = $m } else { $lo = $m } }
    $x1 = $cx + 30 - $hi
    Note "barracks: south face z=$z0, west end x=$x0, east end x=$x1 (player y=$py)"
    # Behind the south-east corner, facing north (a right-handed rifle leads round a right-hand
    # corner). Upright, his muzzle is ~0.51 m right of his origin, leaned right ~0.78 m: the
    # origin 0.69 m inside the corner line puts the upright muzzle 0.18 m behind the corner and
    # the leaned one 0.09 m past it. The target is 1.5 m east of the corner line, 14 m north of
    # the face: hidden until he leans right.
    $sx = $x1 - 0.69; $sz = $z0 - 0.8
    Sqf ("leanPlayer setPos [$sx,$sz,0]; leanPlayer setDir 0; leanTarget setPos [" + ($x1 + 1.5) + "," + ($z0 + 14) + ",0]; leanTarget setDir 180")
    Start-Sleep -Seconds 2
    Note ("target at " + (Eval 'getPos leanTarget').result + ", player at " + (Eval 'getPos leanPlayer').result)

    # --- First person -------------------------------------------------------------------
    Sqf 'leanPlayer switchCamera "INTERNAL"'
    Start-Sleep -Seconds 1
    $null = State 'fp straight'; Shot 'fp-stand-straight'
    Leaned 'fp-stand-left' 20 'fp stand left'
    Leaned 'fp-stand-right' 8 'fp stand right'

    function Tap($sc) { Hold $sc; Start-Sleep -Milliseconds 120; Release $sc; Start-Sleep -Milliseconds 700 }
    # --- A shot around the corner -------------------------------------------------------
    $null = Send @{cmd = 'lean_state'; trails = 1 }
    Sqf 'leanPlayer setDir 0'
    Start-Sleep -Milliseconds 600
    # Upright first: aimed at the target, the bullet meets the wall.
    AimAtTarget
    $null = State 'shot upright'
    foreach ($n in 1..3) { Sqf 'leanPlayer fire "M16"'; Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 300
    Shot 'fp-shot-upright'
    $hit0 = Eval 'damage leanTarget'
    Note "target damage after the upright shot: $($hit0.result)"
    Sqf 'leanTarget setDamage 0'
    Hold 8; Start-Sleep -Milliseconds 900
    AimAtTarget
    $null = State 'shot leaned right'
    foreach ($n in 1..3) { Sqf 'leanPlayer fire "M16"'; Start-Sleep -Milliseconds 350 }
    Start-Sleep -Milliseconds 300
    Shot 'fp-shot-leaned-right'
    Sqf 'leanPlayer fire "M16"'
    Sqf 'leanPlayer switchCamera "EXTERNAL"'
    Start-Sleep -Milliseconds 250
    Shot 'ext-shot-leaned-right'
    Release 8; Start-Sleep -Milliseconds 700
    Sqf 'leanPlayer switchCamera "INTERNAL"'
    $hit1 = Eval 'damage leanTarget'
    Note "target damage after the leaned shot: $($hit1.result)"

    # --- Third person, the game's own camera (key held, as in play) -------------------
    Sqf 'leanPlayer switchCamera "EXTERNAL"'
    Start-Sleep -Seconds 1
    $null = State 'ext straight'; Shot 'ext-stand-straight'
    Leaned 'ext-stand-left' 20 'ext stand left'
    Leaned 'ext-stand-right' 8 'ext stand right'

    # --- Aiming down the sights ----------------------------------------------------------
    Sqf 'leanPlayer switchCamera "GUNNER"'
    Start-Sleep -Seconds 1
    $null = State 'ads straight'; Shot 'ads-stand-straight'
    Leaned 'ads-stand-left' 20 'ads stand left'
    Leaned 'ads-stand-right' 8 'ads stand right'
    Sqf 'leanPlayer switchCamera "INTERNAL"'
    Start-Sleep -Seconds 1

    # --- Tap toggle: a short Q press latches, a second tap releases ----------------------
    Hold 20; Start-Sleep -Milliseconds 120; Release 20; Start-Sleep -Milliseconds 900
    $null = State 'tap latched left'; Shot 'fp-tap-latched-left'
    Hold 20; Start-Sleep -Milliseconds 120; Release 20; Start-Sleep -Milliseconds 900
    $null = State 'tap released'

    # --- A wall on the lean side stops the lean -----------------------------------------
    # A second barracks turned 90 degrees, so a long plain face runs north-south. He stands
    # against it, facing north, the wall on his right: as close as his body (and the plinth)
    # lets him, which is where a right lean, the side his head and rifle are on, meets it.
    $bx = $cx + 30; $bz = $pz - 20
    Sqf ("leanWall2 = ""Barracks"" createVehicle [$bx,$bz,0]; leanWall2 setDir 90; leanWall2 setPos [$bx,$bz,0]")
    Start-Sleep -Seconds 2
    $lo = 0.0; $hi = 30.0
    for ($i = 0; $i -lt 14; $i++) { $m = ($lo + $hi) / 2; if (Probe @(($bx - 30), $h, $bz) @(($bx - 30 + $m), $h, $bz)) { $hi = $m } else { $lo = $m } }
    $wf = $bx - 30 + $hi
    Sqf ("leanPlayer setPos [" + ($wf - 0.55) + ",$bz,0]")
    Start-Sleep -Seconds 1
    TurnTo 0 0
    $ws = State 'wall placed'
    Note "wall test: wall face x=$wf, eye x=$($ws.eye[0]) (eye to wall $($wf - [double]$ws.eye[0]) m), body x=$($ws.position[0])"
    Leaned 'fp-wall-right-blocked' 8 'wall right (into the wall)'
    Leaned 'fp-wall-left-free' 20 'wall left (open side)'
    Sqf 'leanPlayer switchCamera "EXTERNAL"'
    Start-Sleep -Seconds 1
    Leaned 'ext-wall-right-blocked' 8 'ext wall right (into the wall)'
    Leaned 'ext-wall-left-free' 20 'ext wall left (open side)'
    Sqf 'leanPlayer switchCamera "INTERNAL"'
    # Back behind the corner for the portraits.
    Sqf ("leanPlayer setPos [$sx,$sz,0]")
    Start-Sleep -Seconds 1
    TurnTo 0

    # --- Portraits: the game's third-person camera, swung round to his front with freelook ---
    # (numpad * toggles freelook; the mouse then orbits the camera, not the man). Real input
    # throughout, and the camera a player actually has.
    Sqf 'leanPlayer switchCamera "EXTERNAL"'
    Start-Sleep -Milliseconds 600
    Tap 85
    foreach ($n in 1..4) { $null = Send @{cmd = 'mouse_motion'; dx = 160; dy = 0 }; Start-Sleep -Milliseconds 150 }
    Start-Sleep -Milliseconds 600
    foreach ($stance in @('stand', 'crouch')) {
        if ($stance -eq 'crouch') { Sqf 'leanPlayer switchMove "Crouch"'; Start-Sleep -Seconds 2 }
        $null = State ('tp ' + $stance + ' straight'); Shot ('tp-' + $stance + '-straight')
        Leaned ('tp-' + $stance + '-left') 20 ('tp ' + $stance + ' left')
        Leaned ('tp-' + $stance + '-right') 8 ('tp ' + $stance + ' right')
    }
    Tap 85
    Tap 93
    Sqf 'leanPlayer switchCamera "INTERNAL"'
    Start-Sleep -Milliseconds 600
    Leaned 'fp-crouch-left' 20 'fp crouch left'

    $null = Send @{cmd = 'exit' }
    if (!$p.WaitForExit(30000)) { throw 'Exit timed out' }
    Write-Output "Lean capture complete: $out"
} finally {
    if ($client) { $client.Dispose() }
    if (!$p.HasExited) { Stop-Process -Id $p.Id; $p.WaitForExit() }
}
