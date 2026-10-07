<# Installed main-menu ocean scene: boat-free storm swells, native captures, ordinary editor preview
   and menu restoration in the same process/window. Run under scripts/with-game-lock.sh.
   Uses isolated user/cache/content directories and exits only the owned test process. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExpectedCommit,
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 180,
    [switch]$Fullscreen
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Game lock required: run through scripts/with-game-lock.sh' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$stamp = (Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if (!$stamp.Contains($ExpectedCommit)) { throw 'Installed source differs from ExpectedCommit' }
if (Get-Process OpenPoseidon, ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Existing game preserved' }
$out = Join-Path $root ('build/menu-sea/' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
$null = New-Item -ItemType Directory -Path $out -Force
$receipt = @{ installed = $stamp; passed = $false; entryOnly = [bool]$EntryOnly; phases = @() }
$p = $null; $client = $null; $reader = $null; $writer = $null
$overallDeadline = [DateTime]::UtcNow.AddSeconds(4 * $TimeoutSeconds)
$oldEnvironment = @{}
foreach ($name in @('POSEIDON_CACHE_DIR', 'POSEIDON_USER_DIR', 'POSEIDON_USER_CONTENT_DIR')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Require($ok, $message) { if (!$ok) { throw $message } }
function Require-Alive {
    Require ([DateTime]::UtcNow -lt $overallDeadline) 'Overall smoke-test deadline exceeded'
    Require ($p -and !$p.HasExited) 'Owned game process exited during smoke test'
}
function Send($cmd) {
    Require-Alive
    $writer.WriteLine(($cmd | ConvertTo-Json -Depth 10 -Compress))
    do {
        $line = $reader.ReadLine()
        Require $line 'Harness disconnected'
        $reply = $line | ConvertFrom-Json -AsHashtable
    } while ($null -eq $reply.ok)
    @{ time = [DateTime]::UtcNow.ToString('o'); request = $cmd; reply = $reply } |
        ConvertTo-Json -Depth 12 -Compress | Add-Content (Join-Path $out 'harness.jsonl')
    Require $reply.ok $line
    return $reply
}
function Value([string]$code) { return ([string](Send @{ cmd = 'eval'; code = $code }).result).Trim('"') }
function Exec([string]$code) { $null = Send @{ cmd = 'exec'; code = $code }; Start-Sleep -Milliseconds 250 }
function Wait-Display([int]$id, [string]$stage) {
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $actual = Value 'triDisplay'
        if ([int]$actual -eq $id) { return }
        Require ([DateTime]::UtcNow -lt $until) ("${stage}: expected display $id, actual $actual")
        Start-Sleep -Milliseconds 250
    } while ($true)
}
function Snapshot([string]$stage) {
    $p.Refresh()
    $phase = @{ stage = $stage; pid = $p.Id; window = $p.MainWindowHandle.ToInt64();
                display = Value 'triDisplay'; mode = Value 'triGetWindowMode';
                controls = (Send @{ cmd = 'eval'; code = 'triControls' }).result }
    $receipt.phases += $phase
    $null = Send @{ cmd = 'screenshot'; path = (Join-Path $out ($stage + '.png')) }
    Require ($phase.pid -eq $receipt.originalPid) 'Editor flow changed process identity'
    Require ($phase.mode -ceq $receipt.originalMode) 'Editor flow changed the live window mode'
    if ($receipt.originalWindow -ne 0) {
        Require ($phase.window -eq $receipt.originalWindow) 'Editor flow replaced the original game window'
    }
}

try {
    $receipt.binaryTimes = @{
        exe = (Get-Item (Join-Path $game 'OpenPoseidon.exe')).LastWriteTimeUtc.ToString('o')
        dll = (Get-Item (Join-Path $game 'wgpu_renderer.dll')).LastWriteTimeUtc.ToString('o')
    }
    $env:POSEIDON_CACHE_DIR = Join-Path $out 'cache'
    $env:POSEIDON_USER_DIR = Join-Path $out 'user'
    $env:POSEIDON_USER_CONTENT_DIR = Join-Path $out 'content'
    foreach ($path in @($env:POSEIDON_CACHE_DIR, $env:POSEIDON_USER_DIR, $env:POSEIDON_USER_CONTENT_DIR)) {
        $null = New-Item -ItemType Directory -Path $path -Force
    }
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'), "preferredViewDistance=700;`ntextLanguage=""English"";`nvoiceLanguage=""English"";`n")
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $log = Join-Path $out 'engine.log'; $stderr = Join-Path $out 'stderr.log'
    $launchArgs = @('--render=wgpu', '--dev', '--width', '1280', '--height', '720', '--harness', "$port", '--lang', 'English', '--log-file', ('"' + $log + '"'))
    if ($Fullscreen) { $launchArgs += @('--display-mode', 'borderless') } else { $launchArgs += '--window' }
    $p = Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $launchArgs -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Require-Alive
        $client = [Net.Sockets.TcpClient]::new()
        $pending = $client.ConnectAsync('127.0.0.1', $port)
        try { Require ($pending.Wait(1000)) 'Connection attempt timed out' } catch { $client.Dispose(); $client = $null }
        if (!$client) {
            Require ([DateTime]::UtcNow -lt $until) 'Startup harness deadline exceeded'
            Start-Sleep -Milliseconds 250
        }
    } while (!$client)
    $stream = $client.GetStream()
    $stream.ReadTimeout = 60000; $stream.WriteTimeout = 10000
    $reader = [IO.StreamReader]::new($stream)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    Wait-Display 0 'Startup menu'
    $p.Refresh(); $receipt.originalPid = $p.Id; $receipt.originalWindow = $p.MainWindowHandle.ToInt64()
    $receipt.originalMode = Value 'triGetWindowMode'
    $expectedMode = if ($Fullscreen) { 'borderless' } else { 'windowed' }
    Require ($receipt.originalMode -ceq $expectedMode) 'Requested launch display mode was not applied'
    Start-Sleep -Seconds 8
    Snapshot 'startup'
    Require (!(Select-String -LiteralPath (Join-Path $game 'assets/menu/RoughSea.Intro/mission.sqm') -Pattern 'opMenuBoat|vehicle="Boat' -Quiet)) 'Menu mission still defines a boat'
    Require ((Value 'isNull opMenuCamera') -ceq 'false') 'Ocean camera is missing'
    $receipt.cameraBefore = @(Value 'getPos opMenuCamera' | ConvertFrom-Json)
    $receipt.bathymetry = Send @{ cmd = 'water_bathymetry'; x = 2500; z = 3500 }
    $receipt.menuWaterBefore = Send @{ cmd = 'water_probe' }
    Snapshot '00_menu'
    $frameStart = [double](Value 'triFrameCount'); $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds 12
    $receipt.renderedFps = ([double](Value 'triFrameCount') - $frameStart) / $watch.Elapsed.TotalSeconds
    $receipt.cameraAfter = @(Value 'getPos opMenuCamera' | ConvertFrom-Json)
    # getPos height is relative to the moving sea surface; compare horizontal position.
    Require ([Math]::Abs($receipt.cameraAfter[0] - $receipt.cameraBefore[0]) -lt 0.01 -and
             [Math]::Abs($receipt.cameraAfter[1] - $receipt.cameraBefore[1]) -lt 0.01) 'Offshore menu camera moved'
    Snapshot '01_menu_swell'
    # Switch to an ordinary editor mission and back in the original window.
    Exec 'triClick 115'; Wait-Display 51 'Island selector'
    Exec 'triSelectList [101,0]'; Exec 'triClick 1'; Wait-Display 26 'Ordinary editor'
    Exec 'triCursorMoveControl 51'; Exec 'triDblClick 51'; Wait-Display 27 'Unit placement'
    Exec 'triClick 1'; Wait-Display 26 'Unit placed'
    Exec 'triClick 107'; Wait-Display 46 'Ordinary mission preview'
    Start-Sleep -Seconds 3
    $receipt.missionWater = Send @{ cmd = 'water_probe' }
    Snapshot '02_mission'
    $null = Send @{cmd='key';sc=41}; Wait-Display 49 'Pause preview'
    Exec 'triClick 104'; Wait-Display 50 'Abort preview'
    Exec 'triClick 2'; Wait-Display 26 'Return from preview'
    Exec 'triClick 2'; Wait-Display 203 'Editor exit confirmation'
    Exec 'triClick 1'; Wait-Display 0 'Restored title menu'
    Start-Sleep -Seconds 5
    Require ((Value 'isNull opMenuCamera') -ceq 'false') 'Ocean camera did not restart'
    Snapshot '03_restored_menu'
    $null = Send @{ cmd = 'exit' }
    Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal game exit failed'
    Require (!(Select-String -Path @($log, $stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Cannot load (world|local|--test-world)|Re-mount reload failed' -Quiet)) 'Runtime or terrain-loading error logged'
    Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim() -ceq $stamp) 'Installed binary pair changed during smoke test'
    $receipt.savedWaterProfiles = @(Get-ChildItem (Join-Path $env:POSEIDON_USER_DIR 'water-look') -File -ErrorAction SilentlyContinue).Count
    Require ($receipt.savedWaterProfiles -eq 0) 'Transient menu sea was saved into a water profile'
    $receipt.passed = $true
    Write-Host ('Evidence: ' + $out)
} catch {
    $receipt.error = $_.Exception.Message
    throw
} finally {
    if ($p -and !$p.HasExited) {
        if ($writer) { try { $writer.WriteLine('{"cmd":"exit"}'); $null = $p.WaitForExit(10000) } catch {} }
        if (!$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(10000) }
    }
    if ($client) { $client.Dispose() }
    foreach ($name in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process') }
    $receipt | ConvertTo-Json -Depth 14 | Set-Content (Join-Path $out 'result.json')
}


