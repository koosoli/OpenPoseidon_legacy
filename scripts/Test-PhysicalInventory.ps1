<# Installed single-player inventory without a custom invicons asset pack.
   Run under with-game-lock.sh. Uses isolated settings, exits only its own process. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[switch]$Multiplayer,[switch]$CombatAssists)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Game lock required' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$stamp = (Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if (!$stamp.Contains($ExpectedCommit)) { throw 'Installed source differs' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Existing game preserved' }
$out = Join-Path $root ('build/physical-inventory/' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
$mission = Join-Path $out 'inventory.eden'
$null = New-Item -ItemType Directory -Path $mission -Force
Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination $mission
$receipt = @{installed=$stamp; passed=$false; stages=@()}
$receipt.runtime_icons=@()
foreach ($icon in (Get-Content (Join-Path $root 'content/inventory-icons/manifest.json') -Raw | ConvertFrom-Json).icons) {
    $path=Join-Path $game $icon.runtime_paa
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $icon.runtime_sha256) {
        throw "Installed original icon differs: $($icon.category)"
    }
    $receipt.runtime_icons+=@{category=$icon.category;sha256=$icon.runtime_sha256}
}
$old = @{}; $p=$null; $client=$null; $writer=$null
foreach ($name in @('POSEIDON_USER_DIR','POSEIDON_CACHE_DIR','POSEIDON_USER_CONTENT_DIR')) {
    $old[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
    $path = Join-Path $out $name
    $null = New-Item -ItemType Directory -Path $path -Force
    [Environment]::SetEnvironmentVariable($name,$path,'Process')
}
function Require($ok,$message) { if (!$ok) { throw $message } }
function Send($cmd) {
    $writer.WriteLine(($cmd | ConvertTo-Json -Depth 8 -Compress))
    do { $line=$reader.ReadLine(); Require $line 'Harness disconnected'; $reply=$line|ConvertFrom-Json } while ($null -eq $reply.ok)
    $line | Add-Content (Join-Path $out 'harness.jsonl')
    Require $reply.ok $line
    return $reply
}
function Value($code) { return ([string](Send @{cmd='eval';code=$code}).result).Trim('"') }
function Exec($code) { $null=Send @{cmd='exec';code=$code}; Start-Sleep -Milliseconds 220 }
function Key($sc) { $null=Send @{cmd='key';sc=$sc}; Start-Sleep -Milliseconds 350 }
function Display($id) {
    $until=[DateTime]::UtcNow.AddSeconds(15)
    do { $actual=Value 'triDisplay'; if ($actual -eq "$id") { return }; Start-Sleep -Milliseconds 150 } while ([DateTime]::UtcNow -lt $until)
    throw "Expected display $id; got $actual"
}
function Snap($name) {
    $receipt.stages += @{name=$name; weapons=Value 'weapons player'; magazines=Value 'magazines player'; display=Value 'triDisplay'}
    $null=Send @{cmd='screenshot';path=(Join-Path $out ($name+'.png'))}
}
function Pointer($x,$y) { Exec ('triCursorMove ['+($x*2-1).ToString('R',[Globalization.CultureInfo]::InvariantCulture)+','+($y*2-1).ToString('R',[Globalization.CultureInfo]::InvariantCulture)+']') }
function Button($down) { $null=Send @{cmd='mouse_button';button=1;down=$down}; Start-Sleep -Milliseconds 250 }
function Click($x,$y) { Pointer $x $y; Button $true; Button $false }
try {
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0); $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
    $log=Join-Path $out 'engine.log'; $stderr=Join-Path $out 'stderr.log'
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--private','--log-file',('"'+$log+'"'))
    if (!$Multiplayer) { $args+=@('--test-mission',('"'+$mission+'"'),'--test-world-hour','12') }
    $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $until=[DateTime]::UtcNow.AddSeconds(180)
    do {
        Require (!$p.HasExited) 'Startup exited'
        $client=[Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client=$null }
        if (!$client) { Require ([DateTime]::UtcNow -lt $until) 'Startup deadline'; Start-Sleep -Milliseconds 250 }
    } while (!$client)
    $stream=$client.GetStream(); $stream.ReadTimeout=30000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    if ($Multiplayer) {
        Display 0; Exec 'triClick 105'; Display 8; Exec 'triClick 104'; Display 17
        Require ((Value 'triSelectListByData [101,"Eden"]') -ceq 'true') 'Everon server island missing'
        Exec 'triSelectList [102,1]'; Exec 'triClick 1'; Display 67
        Exec 'triSendText [103,"InventoryGuardTest"]'; Exec 'triClick 1'; Display 68
        Exec 'triClick 1'
        $blocked=$false; $until=[DateTime]::UtcNow.AddSeconds(90)
        do {
            $writer.WriteLine('{"cmd":"eval","code":"triDisplay"}')
            do { $line=$reader.ReadLine(); Require $line 'Harness disconnected'; $probe=$line|ConvertFrom-Json } while ($null -eq $probe.ok)
            $line|Add-Content (Join-Path $out 'harness.jsonl')
            if (!$probe.ok -and $line.Contains('developer harness disabled in multiplayer')) { $blocked=$true; break }
            Require $probe.ok $line
            $display=([string]$probe.result).Trim('"')
            if ($display -eq '70') { Exec 'triMpAssignSelfSlot "WEST:1"'; Exec 'triClick 1' }
            elseif ($display -eq '52') { Exec 'triClick 1' }
            Start-Sleep -Milliseconds 300
        } while ([DateTime]::UtcNow -lt $until)
        Require $blocked 'Developer harness was not rejected in the hosted network world'
        $receipt.multiplayerRejection=$probe
        $receipt.ping=Send @{cmd='ping'}
        $null=Send @{cmd='exit'}
        Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal MP exit failed'
        Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'MP runtime error'
        Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim() -ceq $stamp) 'Installed MP pair changed'
        $receipt.passed=$true; Write-Host ('Evidence: '+$out); return
    }
    while ((Value 'triSceneReady') -cne 'OK') { Require ([DateTime]::UtcNow -lt $until) 'Scene deadline'; Start-Sleep -Milliseconds 250 }
    Exec 'player allowDamage false;removeAllWeapons player;player addMagazine "HandGrenade";player addMagazine "HandGrenade";0 setRain 0;0 setFog 0;setAccTime 1'
    $before=Value 'magazines player'
    Key 18; Display 62020; Snap '01_open'
    # At 16:9 the first storage tile is centred here; drag across panes to Ground.
    Pointer 0.681 0.318; Button $true; Pointer 0.12 0.40; Button $false
    $afterDrop=Value 'magazines player'
    Require ((@($before|ConvertFrom-Json)).Count -eq 2) 'Fixture magazines missing'
    Require ((@($afterDrop|ConvertFrom-Json)).Count -eq 1) 'Drag did not drop exactly one magazine'
    Snap '02_drop'
    Click 0.052 0.218
    $afterPickup=Value 'magazines player'
    Require ((@($afterPickup|ConvertFrom-Json)).Count -eq 2) 'Click pickup did not restore magazine'
    Snap '03_pickup'
    Key 14; Snap '04_settings'; Key 14
    Key 41; Display 46
    Key 18; Display 62020; Snap '05_reopen'; Key 18; Display 46
    Exec 'removeAllWeapons player;player addMagazine "M16";player addMagazine "M16";player addWeapon "M16"'
    Key 18; Display 62020; Snap '06_weapon_icons'; Key 41; Display 46
    # A script may remove the exact magazine being dragged. Its tile/ghost must
    # remain safe to draw, and releasing it must not recreate the removed item.
    Exec 'removeAllWeapons player;player addMagazine "HandGrenade"'
    Key 18; Display 62020
    Pointer 0.681 0.318; Button $true; Pointer 0.12 0.40
    Exec 'player removeMagazine "HandGrenade"'
    Snap '07_removed_during_drag'
    Button $false
    Require ((@((Value 'magazines player')|ConvertFrom-Json)).Count -eq 0) 'Removed magazine recreated by stale drag'
    Key 41; Display 46
    Key 18; Display 62020
    Snap '08_removed_drag_reopen'
    Click 0.052 0.218
    Require ((Value 'magazines player') -ceq '[]') 'Removed magazine reappeared'
    Key 41; Display 46
    # Hold a ground item while a mission script fills the player's inventory.
    # Releasing must check current capacity, not the display's pre-drag snapshot.
    Exec 'player addMagazine "HandGrenade"'
    Key 18; Display 62020
    Pointer 0.681 0.318; Button $true; Pointer 0.12 0.40; Button $false
    Pointer 0.052 0.218; Button $true; Pointer 0.70 0.40
    # Ordinary SQF addMagazine obeys stock slots (five AT mines, not eight).
    # M16 leaves the launcher slot free (the M60 occupies it too).
    # 5 * 9.5 kg + M16 3.4 kg + Carl Gustaf 14.2 kg = 65.1 kg, > 60 kg.
    Exec (('player addMagazine "Mine";' * 5)+'player addWeapon "M16";player addWeapon "CarlGustavLauncher"')
    $loaded=Value 'magazines player'
    $loadedWeapons=@((Value 'weapons player')|ConvertFrom-Json)
    Require ((@($loaded|ConvertFrom-Json)).Count -eq 5 -and $loadedWeapons -contains 'M16' -and $loadedWeapons -contains 'CarlGustavLauncher') 'Overweight fixture not created'
    Button $false
    $afterRefused=Value 'magazines player'
    Require ($afterRefused -ceq $loaded) 'Stale capacity allowed overweight pickup'
    Snap '09_live_capacity_refusal'
    Key 41; Display 46
    Exec 'removeAllWeapons player'
    Key 18; Display 62020; Click 0.052 0.218
    Require ((@((Value 'magazines player')|ConvertFrom-Json)).Count -eq 1) 'Refused pickup lost ground item'
    Snap '10_capacity_recovered'; Key 41; Display 46
    $receipt.audit=@{removedDuringDragSafe=$true;liveCapacityRefused=$true;refusedItemRecovered=$true}
    $receipt.before=$before; $receipt.afterDrop=$afterDrop; $receipt.afterPickup=$afterPickup
    if ($CombatAssists) {
        Exec 'removeAllWeapons player;player addMagazine "M16";player addMagazine "GrenadeLauncher";player addMagazine "GrenadeLauncher";player addWeapon "M16GrenadeLauncher";player selectWeapon "M16Muzzle";player setDir 0'
        Start-Sleep -Seconds 2
        Snap '11_bearing_rifle'
        Exec 'auditShooter=player'
        $rifleSlot=(Send @{cmd='ai_combat'}).selected
        Exec 'player selectWeapon "M203Muzzle"'
        Start-Sleep -Seconds 2
        $receipt.launcher=(Send @{cmd='ai_combat'}).selected
        Require ($receipt.launcher -ge 0 -and $receipt.launcher -ne $rifleSlot) 'Grenade muzzle not selected'
        Snap '12_grenade_range'
        $null=Send @{cmd='mouse_button';button=3;down=$true}
        Start-Sleep -Milliseconds 700
        Snap '13_grenade_mark'
        $null=Send @{cmd='mouse_button';button=3;down=$false}
        Key 18; Display 62020; Key 14; Snap '14_combat_settings'
        Click 0.335 0.6885; Click 0.335 0.715; Click 0.335 0.7415
        $assistCfg=Join-Path $env:POSEIDON_USER_DIR 'combat_assists.cfg'
        $disabled=Get-Content -LiteralPath $assistCfg -Raw
        Require ($disabled -match 'bearingCallouts=0' -and $disabled -match 'bearingReadout=0' -and $disabled -match 'grenadeRange=0') 'Combat options did not persist'
        Key 41; Display 46
        Snap '15_combat_assists_disabled'
        Key 18; Display 62020
        Click 0.475 0.781
        $defaults=Get-Content -LiteralPath $assistCfg -Raw
        Require ($defaults -match 'bearingCallouts=1' -and $defaults -match 'bearingReadout=1' -and $defaults -match 'grenadeRange=1') 'Combat reset did not restore defaults'
        Key 41; Display 46; Snap '16_combat_assists_reset'
        $receipt.combatAssists=@{defaultBearing=$true;selectedGrenadeMuzzle=$receipt.launcher;settingsPersisted=$true;resetPassed=$true;visualReview='pending'}
        Exec 'player setPos [5018,4087,0];group player setCombatMode "BLUE";createCenter east;radioEnemyGroup=createGroup east;radioEnemyGroup setCombatMode "BLUE";"SoldierWB" createUnit [[5023,4087,0],group player,"radioFriend=this",1,"PRIVATE"];radioFriend setBehaviour "AWARE";"SoldierEB" createUnit [[5025,4087,0],radioEnemyGroup,"radioEnemy=this",1,"PRIVATE"]'
        $receipt.radioWaves=@()
        for ($sample=0;$sample -lt 16;$sample++) {
            $receipt.radioWaves+=Value 'triRadioWaveStates'
            Start-Sleep -Milliseconds 500
        }
        Snap '17_contact_report'
    }
    $null=Send @{cmd='exit'}
    Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal exit failed'
    Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Runtime error'
    Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim() -ceq $stamp) 'Installed pair changed'
    $receipt.passed=$true; Write-Host ('Evidence: '+$out)
} catch { $receipt.error=$_.Exception.Message; throw } finally {
    if ($p -and !$p.HasExited) {
        if ($writer) { try { $writer.WriteLine('{"cmd":"exit"}'); $null=$p.WaitForExit(10000) } catch {} }
        if (!$p.HasExited) { $p.Kill(); $null=$p.WaitForExit(10000) }
    }
    if ($client) { $client.Dispose() }
    foreach ($name in $old.Keys) { [Environment]::SetEnvironmentVariable($name,$old[$name],'Process') }
    $receipt|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'result.json')
}
