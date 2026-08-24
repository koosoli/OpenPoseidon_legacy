<#
.SYNOPSIS
    Repeatable, non-interactive GPU benchmark driver for the WGPU renderer.

.DESCRIPTION
    Runs one benchmark ARM (a label + a set of environment variables) N times against the
    installed game binary and writes one metrics JSON + PNG + log per repeat, plus an
    arm manifest.  Feed the resulting directories to scripts/farfield-compare.py.

    WHY THIS EXISTS — the thing that bites everyone first:

      --capture-metrics is written ONLY by a screenshot event.  There are exactly four
      call sites of WriteCaptureMetrics in apps/cwr/Game/GameApplication.cpp and all four
      sit immediately after GEngine->Screenshot():
        * menu screenshot mode,
        * --test-mission + --test-type screenshot,
        * --auto-screenshot (both game loops).
      Nothing writes it on exit, on close, or on a timer.

      --duration is NOT a client option at all.  It is read only by
      apps/cwr/Server/ServerApplication.cpp.  Passing "--duration 45 --capture-metrics x"
      to the game therefore runs forever and writes nothing — which is exactly the
      failure this script was written to make impossible.

    So this script always pairs --capture-metrics with a screenshot trigger, in one of
    two proven shapes:

      MISSION MODE (default; byte-for-byte the shape of the -MissionCaptureSmoke arm of
      scripts/run_preview0_wgpu_check.ps1, the only invocation known to work):
        --check --render=wgpu --test-mission M --test-type screenshot --screenshot P
        --capture-metrics J --screenshot-delay N --log-file L --log-level info
      --check bounds startup; screenshot-test mode defers the bounded exit indefinitely
      (Poseidon/UI/DisplayUIMenus.cpp: "deferring exit for requested screenshot"), so a
      large -ScreenshotDelay is safe.  The delay counts GAMEPLAY FRAMES, not seconds.

      WORLD MODE (-World, or any -WarmupSeconds > 0): a wall-clock trigger, because
      substituted worlds stream objects in over 10-20 s and a frame count is not a clock.
        --render=wgpu --window ... --test-mission M --test-world W --test-world-freefly ...
        --auto-screenshot "<S>s:P" --capture-metrics J ...
      --check is deliberately ABSENT here: without --test-type screenshot the bounded
      smoke exits the moment gameplay starts and the timed capture never fires.  The run
      is bounded by this script's own watchdog instead.

    PROCESS HYGIENE (RND-033): this script kills ONLY the PIDs it started, plus their
    direct children.  Every game process that existed before it launched is recorded and
    protected.  It never kills by process NAME.  If a foreign game process is already
    running it refuses to start, because two resident processes inflate every timed
    region by a common factor; -AllowConcurrent downgrades that to a warning and stamps
    the affected repeats as contended so the comparer can drop them.

.EXAMPLE
    # A/B on a stock mission, 3 repeats each
    .\scripts\farfield-bench.ps1 -Label legacy -Env @{ WGR_LOD_GOVERNOR_RANGE = '16'; WGR_OBJECT_STREAM_MS_PER_UPDATE = '0' }
    .\scripts\farfield-bench.ps1 -Label tiered -Env @{ WGR_LOD_GOVERNOR_RANGE = '4';  WGR_OBJECT_STREAM_MS_PER_UPDATE = '4' }
    python .\scripts\farfield-compare.py --arm legacy=.tmp-farfield-bench\legacy --arm tiered=.tmp-farfield-bench\tiered

.NOTES
    Windows PowerShell 5.1.  No &&, ||, ternary or ?? anywhere in this file.
#>
[CmdletBinding()]
param(
    # Name of this arm.  Becomes the output sub-directory and the column header in the comparer.
    [Parameter(Mandatory = $true)]
    [string]$Label,

    # Mission folder (or mission.sqm).  Resolved against, in order: as given, the repo root,
    # the game directory.  The tests/perf/missions/* set is the intended stock corpus.
    [string]$Mission = 'tests/perf/missions/perf_abel.abel',

    # Optional substituted world (.wrp).  Presence of this switches the run to WORLD MODE.
    [string]$World = '',

    # --test-world-freefly X Z Y AZIMUTH ELEVATION.  Exactly 5 values, or none.
    [double[]]$Freefly = @(),

    # --test-world-hour (0..23.99).  Negative means "leave the mission's own time alone".
    [double]$WorldHour = -1,

    # --addon-root <dir>.  A foreign generation's world (Arma 3, DayZ) has no configs in the
    # CWA install, so its models resolve only against the source game's Addons directory.
    # Everon needs none because its models were exported into the game dir.  DayZ Chernarus
    # needs --addon-root "<DayZ>\Addons".
    [string]$AddonRoot = '',

    # --mod <name>, repeatable.  A3 worlds mount through a MOD FOLDER (@a3stratis), and
    # --addon-root REJECTS mod folders - they are not interchangeable.  Getting this wrong
    # is silent: the world streams all its placements in, and the renderer registers a few
    # hundred instances instead of a few hundred thousand, because every model resolved to
    # nothing.  ALWAYS check objects.registered_instances against the census placement count.
    [string[]]$Mod = @(),

    # Output root.  One sub-directory per -Label is created underneath.
    [string]$Out = '',

    # Environment variables applied to the child process for every repeat of this arm.
    # Deliberately not named after any single lever: this is how an A/B arm is defined.
    [Alias('EnvVars')]
    [hashtable]$Env = @{},

    [ValidateRange(1, 25)]
    [int]$Repeats = 3,

    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',

    [ValidateSet('wgpu', 'gl33')]
    [string]$Backend = 'wgpu',

    # 0 means "do not pass the flag" — the default resolution is then whatever the
    # engine picks, which keeps MISSION MODE identical to the proven smoke invocation.
    [int]$Width = 0,
    [int]$Height = 0,

    # MISSION MODE only.  Gameplay frames to render before capturing.
    [int]$ScreenshotDelay = 10,

    # WORLD MODE trigger, in seconds of wall clock.  Negative = auto (0 for a stock
    # mission, 25 for a substituted world, which is past the 10-20 s streaming window).
    [double]$WarmupSeconds = -1,

    # Per-repeat watchdog.  0 = auto.
    [int]$TimeoutSeconds = 0,

    # Proceed even though another game process is already resident (marks repeats contended).
    [switch]$AllowConcurrent,

    # Exit non-zero unless EVERY repeat produced a valid capture (default: tolerate
    # partial failure as long as at least one repeat is valid).
    [switch]$RequireAll
)

$ErrorActionPreference = 'Stop'
# Version 1.0 on purpose: the metrics JSON is read back through ConvertFrom-Json, and
# StrictMode 2.0 turns every "does this capture have that field?" test into a terminating
# error instead of a false. Schema drift must be reported, not thrown.
Set-StrictMode -Version 1.0

$repoRoot = Split-Path -Parent $PSScriptRoot
# $Env is a parameter here; never touch $env: syntax below, use [Environment] instead,
# so there is no chance of the parameter and the environment provider being confused.
$envOverrides = @{}
if ($Env) {
    foreach ($key in $Env.Keys) { $envOverrides[[string]$key] = [string]$Env[$key] }
}

function Resolve-InputPath {
    param([string]$Path, [string]$What)
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$What was not supplied." }
    # Plain arrays throughout this file, never a generic collection. On this host, @() over
    # such a collection throws "the argument types do not match" (reproduced on PowerShell
    # 5.1.26100 with two integers in one), and the error surfaces far from the line that
    # built it.
    $candidates = @()
    if ([IO.Path]::IsPathRooted($Path)) {
        $candidates += $Path
    }
    else {
        $candidates += (Join-Path $repoRoot $Path)
        $candidates += (Join-Path $GameDir $Path)
        $candidates += (Join-Path (Get-Location).Path $Path)
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "$What not found: '$Path' (looked in: $($candidates -join '; '))"
}

function Quote-Arg {
    param([string]$Value)
    return '"' + $Value + '"'
}

# Kill the process we started and its direct children, and NOTHING else.  RND-033 records
# capture scripts killing each other's runs by stopping every PoseidonGame on the box; the
# protected set makes that structurally impossible here.
function Stop-OwnedProcess {
    param([int]$RootProcessId, [hashtable]$Protected)
    $victims = @()
    try {
        $children = Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId=$RootProcessId" -ErrorAction SilentlyContinue
        foreach ($child in $children) { $victims += [int]$child.ProcessId }
    }
    catch {
        Write-Verbose "Could not enumerate children of PID ${RootProcessId}: $($_.Exception.Message)"
    }
    $victims += $RootProcessId
    foreach ($victim in $victims) {
        if ($Protected.ContainsKey($victim)) {
            Write-Warning "Refusing to stop PID $victim - it was running before this benchmark started."
            continue
        }
        Stop-Process -Id $victim -Force -ErrorAction SilentlyContinue
    }
}

function Get-ForeignGameProcessIds {
    param([hashtable]$Owned)
    $ids = @()
    $running = @(Get-Process -Name 'ColdWarAssault', 'PoseidonGame' -ErrorAction SilentlyContinue)
    foreach ($process in $running) {
        if (-not $Owned.ContainsKey([int]$process.Id)) { $ids += [int]$process.Id }
    }
    # PowerShell unrolls this on return, so every CALLER wraps it in @() - otherwise an
    # empty result is $null and .Count on it is a null-reference bug at the worst moment.
    return $ids
}

# Property presence test for ConvertFrom-Json output.  A capture written by an older
# binary is a schema mismatch to report, not an exception to throw.
function Test-JsonProperty {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $false }
    if ($null -eq $Object.PSObject) { return $false }
    return ($null -ne $Object.PSObject.Properties[$Name])
}

# ----------------------------------------------------------------------------------------
# Resolve inputs
# ----------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $GameDir)) { throw "Game directory not found: $GameDir" }
$gameExe = Join-Path $GameDir 'openposeidon.exe'
if (-not (Test-Path -LiteralPath $gameExe)) {
    throw "Game executable not found: $gameExe`n  Deploy first: .\scripts\Deploy.ps1"
}
# A number nobody can attribute to a commit is not a measurement. The ABI handshake will
# NOT catch a version skew - it only trips on a struct-layout change, so a DLL many commits
# from the exe loads perfectly cleanly and simply lacks those commits.
#
# File mtimes cannot answer this: Copy-Item preserves the source timestamp, so an
# incremental deploy that did not need to rebuild the DLL legitimately leaves it hours
# older than the exe. DEPLOYED-FROM.txt is the authority - Deploy.ps1 stamps it with the
# branch and commit of the pair it just wrote, and it is the ONLY record of what these two
# files actually are. Refuse to benchmark an unstamped install, and put the commit in the
# run metadata so the numbers carry it.
$stampPath = Join-Path $GameDir 'DEPLOYED-FROM.txt'
if (-not (Test-Path -LiteralPath $stampPath)) {
    throw "$GameDir has no DEPLOYED-FROM.txt, so there is no record of which commit these binaries are. Deploy with .\scripts\Deploy.ps1 (which stamps it) before benchmarking."
}
$deployedFrom = (Get-Content -LiteralPath $stampPath -TotalCount 1).Trim()
Write-Host "Measuring: $deployedFrom" -ForegroundColor Cyan

if ($Backend -eq 'wgpu') {
    $rendererDll = Join-Path $GameDir 'wgpu_renderer.dll'
    if (-not (Test-Path -LiteralPath $rendererDll)) {
        throw "wgpu_renderer.dll is missing from $GameDir - the exe will fail with 'Entry Point Not Found'."
    }
}

$missionPath = Resolve-InputPath -Path $Mission -What 'Mission'
$worldPath = ''
$worldMode = $false
if (-not [string]::IsNullOrWhiteSpace($World)) {
    $worldPath = Resolve-InputPath -Path $World -What 'World (.wrp)'
    $worldMode = $true
}
$addonRootPath = ''
if (-not [string]::IsNullOrWhiteSpace($AddonRoot)) {
    if (-not (Test-Path -LiteralPath $AddonRoot)) { throw "Addon root not found: $AddonRoot" }
    $addonRootPath = (Resolve-Path -LiteralPath $AddonRoot).Path
    if (-not $worldMode) {
        Write-Warning '-AddonRoot was given without -World; it only applies to a substituted world.'
    }
}
if ($Freefly.Count -ne 0 -and $Freefly.Count -ne 5) {
    throw "-Freefly takes exactly 5 values (X Z Y azimuth elevation); got $($Freefly.Count)."
}
if ($Freefly.Count -eq 5 -and -not $worldMode) {
    Write-Warning '-Freefly was given without -World; the freefly pose only applies to a substituted world.'
}

$warmup = $WarmupSeconds
if ($warmup -lt 0) {
    if ($worldMode) { $warmup = 25 } else { $warmup = 0 }
}
$timedMode = ($warmup -gt 0)

if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $repoRoot '.tmp-farfield-bench' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repoRoot $Out }
$armDir = Join-Path $Out $Label
New-Item -ItemType Directory -Force -Path $armDir | Out-Null

$timeout = $TimeoutSeconds
if ($timeout -le 0) {
    if ($timedMode) { $timeout = [int]($warmup + 180) } else { $timeout = 150 }
}

# Width/Height: default 800x600 in world mode (the resolution every RND-033 figure was
# taken at), untouched in mission mode so the proven invocation stays proven.
$width = $Width
$height = $Height
if ($worldMode) {
    if ($width -le 0) { $width = 800 }
    if ($height -le 0) { $height = 600 }
}

# ----------------------------------------------------------------------------------------
# Contention gate
# ----------------------------------------------------------------------------------------
$ownedPids = @{}
$protectedPids = @{}
$foreignAtStart = @(Get-ForeignGameProcessIds -Owned $ownedPids)
foreach ($foreignPid in $foreignAtStart) { $protectedPids[[int]$foreignPid] = $true }
if ($foreignAtStart.Count -gt 0) {
    $message = "Another game process is already running (PID(s): $($foreignAtStart -join ', ')). " +
    'Two resident processes inflate EVERY timed region by a common factor (RND-033), so these ' +
    'numbers would not be comparable.'
    if (-not $AllowConcurrent) {
        throw "$message Close it, or re-run with -AllowConcurrent to measure anyway."
    }
    Write-Warning "$message Continuing because -AllowConcurrent was given; repeats will be marked contended."
}

Write-Output "=== farfield-bench: arm '$Label' ==="
Write-Output "  game      : $gameExe"
Write-Output "  mission   : $missionPath"
if ($worldMode) { Write-Output "  world     : $worldPath" }
$modeName = 'mission screenshot-test'
if ($timedMode) { $modeName = "timed auto-screenshot (${warmup}s)" }
Write-Output "  mode      : $modeName"
Write-Output "  repeats   : $Repeats"
Write-Output "  out       : $armDir"
if ($envOverrides.Count -eq 0) {
    Write-Output '  env       : (none - engine defaults)'
}
else {
    foreach ($key in ($envOverrides.Keys | Sort-Object)) { Write-Output "  env       : $key=$($envOverrides[$key])" }
}

# ----------------------------------------------------------------------------------------
# Apply arm environment (restored in the finally below)
# ----------------------------------------------------------------------------------------
$savedEnv = @{}
foreach ($key in $envOverrides.Keys) {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, $envOverrides[$key], 'Process')
}

$results = @()
$failures = @()

try {
    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        $stem = Join-Path $armDir ('run-{0:d2}' -f $repeat)
        $jsonPath = "$stem.json"
        $pngPath = "$stem.png"
        $logPath = "$stem.log"
        $stdoutPath = "$stem.stdout.txt"
        $stderrPath = "$stem.stderr.txt"
        $metaPath = "$stem.meta.json"
        Remove-Item -LiteralPath $jsonPath, $pngPath, $logPath, $stdoutPath, $stderrPath, $metaPath -ErrorAction SilentlyContinue

        $argumentParts = @()
        if (-not $timedMode) { $argumentParts += '--check' }
        $argumentParts += "--render=$Backend"
        if ($timedMode) { $argumentParts += '--window' }
        if ($width -gt 0) { $argumentParts += "--width=$width" }
        if ($height -gt 0) { $argumentParts += "--height=$height" }
        $argumentParts += '--test-mission'
        $argumentParts += Quote-Arg $missionPath
        if ($worldMode) {
            $argumentParts += '--test-world'
            $argumentParts += Quote-Arg $worldPath
            if ($Freefly.Count -eq 5) {
                $argumentParts += '--test-world-freefly'
                foreach ($component in $Freefly) {
                    $argumentParts += [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $component)
                }
            }
            if ($WorldHour -ge 0) {
                $argumentParts += '--test-world-hour'
                $argumentParts += [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $WorldHour)
            }
            if (-not [string]::IsNullOrWhiteSpace($addonRootPath)) {
                $argumentParts += '--addon-root'
                $argumentParts += Quote-Arg $addonRootPath
            }
            foreach ($modName in $Mod) {
                $argumentParts += '--mod'
                $argumentParts += $modName
            }
        }
        if ($timedMode) {
            $trigger = [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}s', $warmup)
            $argumentParts += '--auto-screenshot'
            $argumentParts += Quote-Arg ("$trigger" + ':' + $pngPath)
        }
        else {
            $argumentParts += '--test-type'
            $argumentParts += 'screenshot'
            $argumentParts += '--screenshot'
            $argumentParts += Quote-Arg $pngPath
            $argumentParts += '--screenshot-delay'
            $argumentParts += [string]$ScreenshotDelay
        }
        $argumentParts += '--capture-metrics'
        $argumentParts += Quote-Arg $jsonPath
        $argumentParts += '--log-file'
        $argumentParts += Quote-Arg $logPath
        $argumentParts += '--log-level'
        $argumentParts += 'info'
        $argumentString = ($argumentParts -join ' ')

        Write-Output ""
        Write-Output "--- $Label repeat $repeat/$Repeats ---"
        Write-Verbose "openposeidon.exe $argumentString"

        $startedAt = Get-Date
        $process = Start-Process -FilePath $gameExe -ArgumentList $argumentString -WorkingDirectory $GameDir `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
        $ownedPids[[int]$process.Id] = $true
        $exited = $process.WaitForExit($timeout * 1000)
        $timedOut = $false
        if (-not $exited) {
            $timedOut = $true
            Write-Warning "Repeat $repeat exceeded the ${timeout}s watchdog; stopping PID $($process.Id) only."
            Stop-OwnedProcess -RootProcessId ([int]$process.Id) -Protected $protectedPids
            $process.WaitForExit(10000) | Out-Null
        }
        $elapsed = ((Get-Date) - $startedAt).TotalSeconds
        $exitCode = -1
        try { $exitCode = [int]$process.ExitCode } catch { $exitCode = -1 }

        $foreignNow = @(Get-ForeignGameProcessIds -Owned $ownedPids)
        $contended = ($foreignNow.Count -gt 0 -or $foreignAtStart.Count -gt 0)
        if ($foreignNow.Count -gt 0) {
            Write-Warning "A foreign game process (PID(s): $($foreignNow -join ', ')) was resident during repeat $repeat; marking it contended."
        }

        # ---- validation: fail loudly, never report zeros --------------------------------
        $problems = @()
        if ($timedOut) { $problems += "watchdog fired after ${timeout}s" }
        if ($exitCode -ne 0 -and -not $timedOut) { $problems += "exit code $exitCode" }
        if (-not (Test-Path -LiteralPath $jsonPath)) {
            $problems += "no metrics JSON at $jsonPath (--capture-metrics is written ONLY by a screenshot event; check the log for 'Capture metrics saved')"
        }
        if (-not (Test-Path -LiteralPath $pngPath)) { $problems += "no screenshot at $pngPath" }

        $frameTotal = $null
        $attributed = $null
        $metrics = $null
        if ($problems.Count -eq 0) {
            try {
                $metrics = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
            }
            catch {
                $problems += "metrics JSON is malformed: $($_.Exception.Message)"
            }
        }
        if ($problems.Count -eq 0 -and $null -ne $metrics) {
            if (-not (Test-JsonProperty -Object $metrics -Name 'gpu_timestamps_available')) {
                $problems += 'the capture has no gpu_timestamps_available field - schema mismatch with this binary'
            }
            elseif (-not $metrics.gpu_timestamps_available) {
                $problems += 'gpu_timestamps_available is false - the adapter reported no timestamp queries, so every region would read 0. Refusing to report that as a measurement.'
            }
            if (-not (Test-JsonProperty -Object $metrics -Name 'gpu_timings_ms') -or
                @($metrics.gpu_timings_ms).Count -eq 0) {
                $problems += 'gpu_timings_ms is missing or empty'
            }
            else {
                $frameRow = @($metrics.gpu_timings_ms | Where-Object { $_.name -eq 'GPU frame total' })
                if ($frameRow.Count -eq 0) {
                    $problems += "no 'GPU frame total' region in the capture"
                }
                elseif ($frameRow[0].milliseconds -le 0) {
                    $problems += "GPU frame total is $($frameRow[0].milliseconds) ms - not a measured frame"
                }
                else {
                    $frameTotal = [double]$frameRow[0].milliseconds
                    $sum = 0.0
                    foreach ($row in $metrics.gpu_timings_ms) {
                        # Leaves point at their container, so containers are counted exactly once.
                        # The frame-total row is excluded BY NAME rather than by trusting its
                        # contained_by: captures from older binaries report the whole-frame
                        # envelope as outermost, and summing it there yields an "attributed"
                        # larger than the frame and a negative residual.
                        if ($row.name -eq 'GPU frame total') { continue }
                        if ([int]$row.contained_by -eq -1 -and [double]$row.milliseconds -gt 0) {
                            $sum += [double]$row.milliseconds
                        }
                    }
                    $attributed = $sum
                }
            }
            if ($Backend -eq 'wgpu' -and (Test-JsonProperty -Object $metrics -Name 'renderer') -and
                $metrics.renderer -notlike '*WGPU*') {
                $problems += "renderer is '$($metrics.renderer)' but wgpu was requested"
            }
        }

        $status = 'ok'
        if ($problems.Count -gt 0) { $status = 'failed' }

        # PSCustomObject, not a bare [ordered] hashtable: Where-Object and Sort-Object
        # below need real properties, and a dictionary key is not one.
        $meta = [PSCustomObject][ordered]@{
            label            = $Label
            repeat           = $repeat
            # Which binaries produced these numbers. Without this a metrics JSON is an
            # orphan: it cannot be attributed to a commit after the fact.
            deployed_from    = $deployedFrom
            status           = $status
            contended        = $contended
            timed_out        = $timedOut
            exit_code        = $exitCode
            elapsed_seconds  = [math]::Round($elapsed, 2)
            started_at       = $startedAt.ToString('o')
            mode             = $modeName
            env              = $envOverrides
            arguments        = $argumentString
            metrics_path     = $jsonPath
            screenshot_path  = $pngPath
            log_path         = $logPath
            gpu_frame_ms     = $frameTotal
            attributed_ms    = $attributed
            problems         = @($problems)
        }
        ($meta | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $metaPath -Encoding UTF8

        if ($problems.Count -gt 0) {
            foreach ($problem in $problems) { Write-Warning "repeat ${repeat}: $problem" }
            $tail = ''
            if (Test-Path -LiteralPath $logPath) {
                $tail = (Get-Content -LiteralPath $logPath -Tail 12 -ErrorAction SilentlyContinue) -join "`n"
            }
            if ($tail) { Write-Output "  last log lines:`n$tail" }
            $failures += "repeat ${repeat}: $($problems -join '; ')"
        }
        else {
            $residual = $frameTotal - $attributed
            $residualPercent = 0.0
            if ($frameTotal -gt 0) { $residualPercent = ($residual / $frameTotal) * 100.0 }
            $contendedNote = ''
            if ($contended) { $contendedNote = '  [CONTENDED]' }
            Write-Output ("  frame {0:F3} ms | attributed {1:F3} | residual {2:F3} ({3:F1}%) | {4:F1}s wall{5}" -f `
                    $frameTotal, $attributed, $residual, $residualPercent, $elapsed, $contendedNote)
            # Print the instance count every repeat.  A world whose models did not mount
            # (wrong --mod / --addon-root) still streams every placement in and still
            # produces a perfectly valid, perfectly meaningless capture; the ONLY cheap
            # tell is that registered_instances is a few hundred instead of a few hundred
            # thousand.  Two runs were lost to this before it was printed.
            if (Test-JsonProperty -Object $metrics -Name 'objects') {
                Write-Output ("  registered instances {0} | main {1} | main tris {2}" -f `
                        $metrics.objects.registered_instances, $metrics.objects.main_instances, $metrics.objects.main_tris)
            }
        }
        $results += $meta
    }
}
finally {
    foreach ($key in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], 'Process')
    }
    foreach ($ownedPid in @($ownedPids.Keys)) {
        $leftover = Get-Process -Id $ownedPid -ErrorAction SilentlyContinue
        if ($leftover) {
            Write-Warning "Cleaning up leftover benchmark process PID $ownedPid."
            Stop-OwnedProcess -RootProcessId ([int]$ownedPid) -Protected $protectedPids
        }
    }
}

$valid = @($results | Where-Object { $_.status -eq 'ok' })
$manifest = [PSCustomObject][ordered]@{
    label        = $Label
    generated_at = (Get-Date).ToString('o')
    game_exe     = $gameExe
    exe_written  = (Get-Item -LiteralPath $gameExe).LastWriteTime.ToString('o')
    mission      = $missionPath
    world        = $worldPath
    addon_root   = $addonRootPath
    mods         = @($Mod)
    freefly      = @($Freefly)
    world_hour   = $WorldHour
    backend      = $Backend
    mode         = $modeName
    width        = $width
    height       = $height
    repeats      = $Repeats
    valid        = $valid.Count
    env          = $envOverrides
    runs         = @($results)
}
$manifestPath = Join-Path $armDir 'arm.json'
($manifest | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Output ""
Write-Output "=== arm '$Label': $($valid.Count)/$Repeats valid captures -> $armDir ==="
if ($valid.Count -gt 0) {
    $cheapest = ($valid | Sort-Object -Property gpu_frame_ms)[0]
    Write-Output ("  cheapest steady frame: {0:F3} ms (repeat {1})" -f $cheapest.gpu_frame_ms, $cheapest.repeat)
}

# Write-Error, not throw: $ErrorActionPreference is Stop, so a thrown error would abandon
# the script before its own exit code could be set, and the caller would see 1 for every
# distinct failure. -ErrorAction Continue keeps the exit codes meaningful (2 = nothing
# measured, 3 = a repeat failed under -RequireAll).
if ($valid.Count -eq 0) {
    Write-Error -ErrorAction Continue -Message "Arm '$Label' produced no valid capture. $($failures -join ' | ')"
    exit 2
}
if ($RequireAll -and $valid.Count -ne $Repeats) {
    Write-Error -ErrorAction Continue -Message "Arm '$Label' produced $($valid.Count)/$Repeats valid captures and -RequireAll was given. $($failures -join ' | ')"
    exit 3
}
exit 0
