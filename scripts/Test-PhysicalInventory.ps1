<# Installed single-player inventory without a custom invicons asset pack.
   Run under with-game-lock.sh. Uses isolated settings, exits only its own process. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[switch]$Multiplayer)
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
    $receipt.before=$before; $receipt.afterDrop=$afterDrop; $receipt.afterPickup=$afterPickup
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
