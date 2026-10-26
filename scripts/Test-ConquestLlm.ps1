<#
.SYNOPSIS
    Conquest's LLM commander and HQ conversation, against a local stand-in model (no API key, no cost).

.DESCRIPTION
    Runs the worktree's dist build (nothing is deployed) on the Everon war, with NPC chat pointed at
    scripts/conquest_mock_llm.py (an OpenAI-compatible endpoint on localhost) as the "Custom" provider:
      good     EAST's commander is the LLM: its plan is applied (log, posture, purchase), the next request
               carries its memory of the earlier decision, and the player asking HQ (NPC chat) to push
               Figari becomes HQ's main effort.
      garbage  the model answers with prose: the answer is rejected and the standard commander carries on.
      down     nothing listens on the port: the request fails and the standard commander carries on.
    Writes build/conquest-runs/llm-<time>/<case>/. Exit code 1 on a failed check.
    Must run under the game lock; it takes it itself when LOCK_OWNER is not set.
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
    $env:LOCK_OWNER = 'conquest LLM test'
    try { & 'C:\Program Files\Git\bin\bash.exe' @argv; exit $LASTEXITCODE }
    finally { Remove-Item Env:LOCK_OWNER }
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (!$Bin) { $Bin = Join-Path $root 'dist/x64-win-rwdi' }
$exe = Join-Path $Bin 'OpenPoseidon.exe'
$base = Join-Path $root ('build/conquest-runs/llm-' + (Get-Date -Format yyyyMMdd-HHmmss))
$mission = Join-Path $root 'content/default-packs/conquest/missions/conquest-everon.Eden'
$script:failures = @()
function Check($ok, $what) {
    if ($ok) { Write-Output "PASS $what" } else { Write-Output "FAIL $what"; $script:failures += $what }
}
function FreePort {
    $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $l.Start(); $p = $l.LocalEndpoint.Port; $l.Stop(); return $p
}

function Invoke-Case($name, $mode, [scriptblock]$body) {
    $out = Join-Path $base $name
    New-Item -ItemType Directory -Force $out | Out-Null
    $script:modelLog = Join-Path $out 'model-requests.txt'
    $modelPort = FreePort
    $mock = $null
    if ($mode -ne 'down') {
        $mock = Start-Process -FilePath 'python' -ArgumentList @((Join-Path $PSScriptRoot 'conquest_mock_llm.py'), "$modelPort", $mode, ('"' + $script:modelLog + '"')) -PassThru -WindowStyle Hidden
    }
    $env:POSEIDON_USER_DIR = Join-Path $out 'user'
    New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
    [IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), @('qualityPreset=1;', 'dlssMode=0;', 'vsync=0;', 'fpsCap=60;'))
    # NPC chat: the Custom provider at the stand-in model (no key: the Custom provider needs none).
    $npc = @{ enabled = $true; llm = 5; llmBaseUrl = "http://127.0.0.1:$modelPort/v1"; llmModel = 'mock'; stt = 0; tts = 0; requestTimeoutSeconds = 15 }
    [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'npcChat.json'), ($npc | ConvertTo-Json -Compress))
    $env:POSEIDON_RNG_SEED = '21'
    $port = FreePort
    $script:log = Join-Path $out 'engine.log'
    $gameArgs = @('-C', ('"' + $GameDir + '"'), '--render=wgpu', '--window', '--no-dev', '--width', '1280', '--height', '720',
        '--harness', "$port", '--test-mission', ('"' + $mission + '"'), '--log-file', ('"' + $script:log + '"'))
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
        if ($mock -and !$mock.HasExited) { Stop-Process -Id $mock.Id }
    }
}
function Send($command) {
    $script:writer.WriteLine(($command | ConvertTo-Json -Compress -Depth 6))
    do { $line = $script:reader.ReadLine(); if ($null -eq $line) { throw 'Harness closed' }; $r = $line | ConvertFrom-Json } while ($null -eq $r.ok)
    return $r
}
function Eval($code) { return "$((Send @{cmd = 'eval'; code = $code }).result)".Trim('"') }
function LogText { return (Get-Content $script:log -Raw) }
function WaitFor($pattern, $seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((LogText) -match $pattern) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

Invoke-Case 'good' 'good' {
    Check ((Eval 'conquestStart ["WEST", "soldier", "contacts", "noview", ["EAST"]]') -eq 'true') 'good: the war starts with an LLM commander for EAST'
    $null = Eval 'setAccTime 4'
    Check (WaitFor 'EAST LLM commander \([0-9]+ s, attack\): Tyrone first' 60) 'good: the model''s plan is applied (posture attack, its reason in the log)'
    # Squads may already be on their way to Tyrone: the one the plan adds is "reinforcing the main effort".
    Check (WaitFor 'EAST group [0-9]+: attack Tyrone \((main effort|reinforcing the main effort)\)' 90) 'good: Tyrone becomes EAST''s main effort'
    Check (WaitFor 'EAST HQ situation report: We hold the airfield' 30) 'good: the commander radios its situation report'
    Check (WaitFor 'EAST bought a Motorised squad' 150) 'good: the purchase the model asked for is made'
    # The next request (after about four game minutes) carries the commander's memory.
    $deadline = [DateTime]::UtcNow.AddSeconds(150)
    while ([DateTime]::UtcNow -lt $deadline -and !((Test-Path $script:modelLog) -and ((Get-Content $script:modelLog -Raw) -match 'Your earlier decisions'))) { Start-Sleep -Seconds 3 }
    $requests = if (Test-Path $script:modelLog) { Get-Content $script:modelLog -Raw } else { '' }
    Check ($requests -match 'Your earlier decisions[\s\S]*main effort Tyrone') 'good: the second request remembers the first decision'
    Check ($requests -notmatch 'WEST group [0-9]+:') 'good: EAST''s report holds none of WEST''s orders (fog of war)'
    # The player asks HQ (NPC chat) to push Figari.
    $null = Eval 'setAccTime 1'
    $report = Send @{cmd = 'npc_chat_report' }
    Check ("$($report.report)" -match 'CONQUEST: THE WAR AS YOUR SIDE KNOWS IT') 'good: NPC chat''s situation report carries the war'
    $null = Send @{cmd = 'npc_chat_say'; text = 'HQ, put everything on Figari.' }
    Check (WaitFor 'WEST HQ: the player asked for Figari as the main effort' 40) 'good: the player''s request to HQ becomes WEST''s main effort'
    $t = Send @{cmd = 'npc_chat_transcript' }
    Check ((($t.lines | ForEach-Object { $_.text }) -join "`n") -match 'Figari') 'good: HQ answers in the transcript'
}

Invoke-Case 'garbage' 'garbage' {
    Check ((Eval 'conquestStart ["WEST", "soldier", "contacts", "noview", ["EAST"]]') -eq 'true') 'garbage: the war starts'
    $null = Eval 'setAccTime 4'
    Check (WaitFor 'EAST LLM commander: the answer holds no JSON object; the standard commander carries on' 60) 'garbage: the answer is rejected'
    Check (WaitFor 'EAST group [0-9]+: attack' 60) 'garbage: the standard commander keeps giving orders'
}

Invoke-Case 'down' 'down' {
    Check ((Eval 'conquestStart ["WEST", "soldier", "contacts", "noview", ["EAST"]]') -eq 'true') 'down: the war starts'
    $null = Eval 'setAccTime 4'
    Check (WaitFor 'EAST LLM commander: .*the standard commander carries on' 90) 'down: the failed request is logged'
    Check (WaitFor 'EAST group [0-9]+: attack' 60) 'down: the standard commander keeps giving orders'
}

Write-Output "Conquest LLM test: $base"
if ($script:failures.Count) { Write-Output "$($script:failures.Count) check(s) failed"; exit 1 }
Write-Output 'all checks passed'
