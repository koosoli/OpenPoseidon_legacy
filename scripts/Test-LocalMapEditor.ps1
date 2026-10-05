<#
.SYNOPSIS
    Installed-game smoke test: local browser -> native mission editor -> save/load/preview -> restored menu.
.DESCRIPTION
    Run under scripts/with-game-lock.sh after deployment. Uses the same TCP harness helpers and
    isolated user/cache paths as Test-LocalBrowser.ps1. Source archives and existing profiles remain untouched.
    The default flow places an OFP SoldierWB through the editor UI and saves a real .OP_Local_* mission.
    -EntryOnly captures editor entry and restoration while skipping unit placement/save/preview.
.EXAMPLE
    pwsh -File scripts/Test-LocalMapEditor.ps1 -ExpectedCommit <installed-commit> -MapName Stratis
.EXAMPLE
    pwsh -File scripts/Test-LocalMapEditor.ps1 -ExpectedCommit <installed-commit> -MapName Everon -MapGame 'Arma Reforger' -Fullscreen
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExpectedCommit,
    [string]$MapName = 'Stratis',
    [string]$MapGame = '',
    [ValidatePattern('^[A-Za-z0-9_]+$')][string]$MissionName = ('LocalMapEditorSmoke_' + (Get-Date -Format yyyyMMddHHmmss)),
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 300,
    [switch]$Fullscreen,
    [switch]$EntryOnly
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Game lock required: run through scripts/with-game-lock.sh' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$stamp = (Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if (!$stamp.Contains($ExpectedCommit)) { throw 'Installed source differs from ExpectedCommit' }
if (Get-Process OpenPoseidon, ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Existing game preserved' }
$out = Join-Path $root ('build/local-map-editor/' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
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
function Remount-Count {
    if (!(Test-Path $log)) { return 0 }
    return @(Select-String -LiteralPath $log -Pattern 'Re-mount complete').Count
}
function Wait-Remount([int]$count, [string]$stage) {
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ((Remount-Count) -lt $count) {
        Require-Alive
        Require ([DateTime]::UtcNow -lt $until) ("${stage}: remount completion was not logged")
        Start-Sleep -Milliseconds 250
    }
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
function Island-Rows {
    $rows = @()
    $count = [int](Value 'triListSize 101')
    Require ($count -gt 0) 'Original OFP islands are unavailable'
    for ($i = 0; $i -lt $count; $i++) { $rows += Value ("triListText [101,$i]") }
    return ,$rows
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
    Snapshot '00_menu'
    $initialRemounts = Remount-Count

    # Pin original island enumeration so restoration can detect alias leakage or lost OFP content.
    Exec 'triClick 115'; Wait-Display 51 'Original island selector'
    $receipt.originalIslands = Island-Rows
    Exec 'triClick 2'; Wait-Display 0 'Return from original island selector'
    Exec 'triClick 62000'
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $status = Value 'triControlText 62005'
        if ($status -match 'maps found') { break }
        Require ([DateTime]::UtcNow -lt $until) ('Catalog deadline: ' + $status)
        Start-Sleep -Milliseconds 300
    } while ($true)
    $receipt.catalogStatus = $status
    $count = [int](Value 'triListSize 62001'); $index = -1; $matchingRows = @()
    for ($i = 0; $i -lt $count; $i++) {
        $row = Value ("triListText [62001,$i]")
        if ($row -match ('\| ' + [regex]::Escape($MapName) + ' \|') -and
            (!$MapGame -or $row.StartsWith($MapGame + ' |', [StringComparison]::OrdinalIgnoreCase)) -and
            $row -notmatch '\(unsupported\)') {
            $matchingRows += $row; $index = $i
        }
    }
    Require ($matchingRows.Count -eq 1) ("Expected one supported '$MapName' terrain; found $($matchingRows.Count). Use -MapGame to disambiguate.")
    $receipt.selected = $matchingRows[0]
    Snapshot '01_catalog'
    Exec ("triSelectList [62001,$index]")
    Exec 'triClick 62004'
    Wait-Remount ($initialRemounts + 1) 'Mount selected terrain'
    Wait-Display 26 'Local map editor entry'
    Require ((Value 'triGetControlVisible 51') -in @('true','1')) 'Native editor 2D map control is missing'
    Snapshot '02_editor_2d'
    $descriptors = @(Get-ChildItem (Join-Path $env:POSEIDON_USER_DIR 'LocalMapWorlds') -Filter '*.json' -File)
    Require ($descriptors.Count -eq 1) 'Expected one owned local terrain descriptor'
    $receipt.worldId = $descriptors[0].BaseName
    $receipt.descriptor = Get-Content $descriptors[0].FullName -Raw | ConvertFrom-Json -AsHashtable
    Require ($receipt.worldId -match '^OP_Local_[0-9a-f]{16}$') 'Terrain lacks its stable local world identifier'

    if (!$EntryOnly) {
        # Follow the same actual unit-dialog/keyboard paths as the integration editor tests.
        Exec 'triCursorMoveControl 51'; Exec 'triDblClick 51'; Wait-Display 27 'Insert unit dialog'
        Snapshot '03_unit_dialog'
        Require ((Value 'triControlText 102') -ceq 'West') 'Default unit side differs'
        Require ((Value 'triControlText 107') -ceq 'Men') 'Default unit class differs'
        Require ((Value 'triControlText 103') -ceq 'Soldier') 'Default OFP soldier differs'
        Require ((Value 'triControlText 105') -ceq 'Player') 'Default unit is not playable'
        Exec 'triClick 1'; Wait-Display 26 'Placed player unit'
        Snapshot '04_editor_unit'
        Exec 'triClick 102'; Wait-Display 29 'Save mission dialog'
        Require ((Value 'triGetControlFocused 101') -in @('true','1')) 'Save name field lacks keyboard focus'
        Exec ('triTypeText "' + $MissionName + '"')
        Require ((Value 'triControlText 101') -ceq $MissionName) 'Typed mission name was not entered'
        Exec 'triClick 1'; Wait-Display 26 'Saved mission'
        $saved = @(Get-ChildItem (Join-Path $env:POSEIDON_USER_CONTENT_DIR 'missions') -Directory -Filter ($MissionName + '.OP_Local_*'))
        Require ($saved.Count -eq 1) 'Editor did not save one mission under the stable terrain alias'
        Require ($saved[0].Name -ceq ($MissionName + '.' + $receipt.worldId)) 'Saved mission lost the selected terrain identity'
        $sqm = Join-Path $saved[0].FullName 'mission.sqm'
        $text = Get-Content $sqm -Raw
        Require ($text -match 'vehicle\s*=\s*"SoldierWB"') 'Saved mission lacks the OFP soldier'
        Require ($text -match 'player\s*=\s*"PLAYER COMMANDER"') 'Saved soldier is not the mission player'
        $receipt.savedMission = $sqm
        Exec 'triClick 101'; Wait-Display 30 'Reload mission dialog'
        Require ((Value 'triControlText 101') -ceq $MissionName) 'Saved mission is missing from the load list'
        Snapshot '05_saved_mission_list'
        Exec 'triClick 1'; Wait-Display 26 'Reloaded local mission'
        Exec 'triClick 107'; Wait-Display 46 'Mission preview'
        Require ((Value '!isNull player') -ceq 'true') 'Preview did not create a playable OFP soldier'
        $receipt.preview = Send @{ cmd = 'diag_inspect'; unit = 'player' }
        Start-Sleep -Seconds 5 # Allow ordinary object streaming before the visual receipt.
        Snapshot '06_preview'
        Exec 'triSendKey 41'; Wait-Display 49 'Preview pause menu'
        Exec 'triClick 104'; Wait-Display 50 'Abort preview dialog'
        Exec 'triClick 2'; Wait-Display 26 'Return from preview to editor'
        Snapshot '07_returned_editor'
        $receipt.missionRoundTripPassed = $true
    }

    Exec 'triClick 2'; Wait-Display 203 'Editor exit confirmation'
    Exec 'triClick 1'
    # The deferred restore briefly exposes a menu before remounting the base content.
    Wait-Remount ($initialRemounts + 2) 'Restore base content'
    Wait-Display 0 'Restored OFP main menu'
    Exec 'triClick 115'; Wait-Display 51 'Restored island selector'
    $receipt.restoredIslands = Island-Rows
    Require (($receipt.originalIslands | ConvertTo-Json -Compress) -ceq ($receipt.restoredIslands | ConvertTo-Json -Compress)) 'Base island availability differs after exiting the local terrain'
    Snapshot '09_restored_islands'
    Exec 'triClick 2'; Wait-Display 0 'Restored menu after island check'
    Snapshot '08_restored_menu'
    $null = Send @{ cmd = 'exit' }
    Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal game exit failed'
    Require (!(Select-String -Path @($log, $stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Cannot load (world|local|--test-world)|Re-mount reload failed' -Quiet)) 'Runtime or terrain-loading error logged'
    Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim() -ceq $stamp) 'Installed binary pair changed during smoke test'
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
