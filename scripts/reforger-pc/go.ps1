# Reforger parity harness (MrDudeX-reforger). Lives in <CWA>\_OpenPoseidon-Reforger on the test PC.
#
# MODE=poses      Open Poseidon on native Arma Reforger Everon: one launch, the camera visits
#                 every pose of POSES (poses.txt) and takes one screenshot each.
# MODE=workbench  The same poses in the original: Arma Reforger Tools' World Editor with the
#                 MDXParity capture plugin (workbench\MDXParity, public script API only).
#
# Never touches the shared Remastered binaries: the build under test is bin\ (its own exe and
# wgpu_renderer.dll), run with Remastered as working directory, and a private user dir (user\).
# Settings in job.txt or a request file (KEY=value). Writes go.status and shots\<TAG>\.
param([string]$JobFile = 'job.txt')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$cwa = Split-Path $here -Parent
$game = Join-Path $cwa 'Remastered'
$job = @{ MODE='poses'; WAIT='7200'; QUIET='60'; DEPLOY='0'; ZIP=''; POSES='poses.txt'; ONLY=''; TAG='';
          HOUR='11'; WIDTH='1920'; HEIGHT='1080'; FIRST='75'; SETTLE='15'; RENDER='wgpu'; EXTRA='';
          EXE='bin'; FOV='0.7'; FOVY='55.4'; KEEPBMP='0'; WBTIMEOUT='2700';
          REFORGER='H:\SteamLibrary\steamapps\common\Arma Reforger\addons';
          TOOLS='H:\SteamLibrary\steamapps\common\Arma Reforger Tools' }
$jobPath = if ([IO.Path]::IsPathRooted($JobFile)) { $JobFile } else { Join-Path $here $JobFile }
if (Test-Path $jobPath) {
  Get-Content $jobPath | ForEach-Object { if ($_ -match '^\s*([A-Za-z0-9_]+)\s*=(.*)$') { $job[$matches[1]] = $matches[2].Trim() } }
}
$status = Join-Path $here 'go.status'
function St($s) { "$(Get-Date -Format 'HH:mm:ss') $s" | Add-Content $status }
Set-Content $status "start $(Get-Date -Format s) mode=$($job.MODE)"
Remove-Item (Join-Path $here 'STOP') -ErrorAction SilentlyContinue
$inv = [Globalization.CultureInfo]::InvariantCulture

# One game at a time (CLAUDE.md rule 0d): an OpenPoseidon of another session or of the owner is
# waited for; the run starts once none has been seen for QUIET seconds.
function Wait-GameFree {
  $quiet = [int]$job.QUIET; $deadline = (Get-Date).AddSeconds([int]$job.WAIT); $free = $null; $said = $false
  while ($true) {
    if (Test-Path (Join-Path $here 'STOP')) { St 'STOPPED while waiting'; exit 1 }
    # Another session's harness mid-run (a sibling _OpenPoseidon-* with request.running) counts as
    # busy too: its runs are one game process per mission, and the gaps between them are not free.
    $siblingRun = Get-ChildItem $cwa -Directory -Filter '_OpenPoseidon-*' -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -ne $here -and (Test-Path (Join-Path $_.FullName 'request.running')) }
    if ((Get-Process OpenPoseidon -ErrorAction SilentlyContinue) -or $siblingRun) {
      $free = $null
      if (-not $said) { St 'BUSY: OpenPoseidon laeuft schon, warte'; $said = $true }
      if ((Get-Date) -gt $deadline) { St 'BUSY: gave up waiting'; exit 1 }
    } elseif (-not $free) { $free = Get-Date }
    elseif (((Get-Date) - $free).TotalSeconds -ge $quiet) { if ($said) { St 'free' }; return }
    Start-Sleep 5
  }
}

function Read-Poses {
  $list = @()
  foreach ($line in Get-Content (Join-Path $here $job.POSES)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    # A line starting with '!' is raw SQS, run at that point of the sequence (test commands:
    # logInfo format ["...", triEnfusionDoor [x, z, r]], triEnfusionDoorSet [i, 1], ...).
    if ($t.StartsWith('!')) { $list += [pscustomobject]@{ Name = ''; Raw = $t.Substring(1).Trim() }; continue }
    $p = $t -split '\s+'
    if ($p.Count -lt 6) { continue }
    $abs = $p[3].EndsWith('A')
    $list += [pscustomobject]@{ Name = $p[0]; Raw = ''; X = $p[1]; Z = $p[2]; Alt = $p[3].TrimEnd('A'); Abs = $abs; Az = $p[4]; El = $p[5] }
  }
  if ($job.ONLY -ne '') {
    $only = $job.ONLY.Split(',') | ForEach-Object { $_.Trim() }
    $list = @($list | Where-Object { $_.Raw -ne '' -or $only -contains $_.Name })
  }
  return ,$list
}

# Queue: queue\*.txt are further requests. Whatever way this run ends, the oldest of them becomes
# request.txt, which the watcher then runs -- so several runs can be lined up at once.
function Advance-Queue {
  $q = Get-ChildItem (Join-Path $here 'queue') -Filter '*.txt' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
  if ($q -and -not (Test-Path (Join-Path $here 'request.txt'))) {
    Move-Item $q.FullName (Join-Path $here 'request.txt') -Force
    St "queued next: $($q.Name)"
  }
}

try {
$tag = if ($job.TAG -ne '') { $job.TAG } else { Get-Date -Format 'MMdd-HHmm' }
$poses = Read-Poses
if ($poses.Count -eq 0) { St 'NO POSES'; exit 1 }

if ($job.MODE -eq 'workbench') {
  Wait-GameFree
  $docs = [Environment]::GetFolderPath('MyDocuments')
  $wbRoot = Join-Path $docs 'My Games\ArmaReforgerWorkbench'
  $proj = Join-Path $wbRoot 'addons\MDXParity'
  New-Item -ItemType Directory -Force $proj | Out-Null
  Copy-Item (Join-Path $here 'workbench\MDXParity\*') $proj -Recurse -Force
  $sb = New-Object Text.StringBuilder
  [void]$sb.AppendLine('// Generated by go.ps1 from poses.txt -- do not edit.')
  [void]$sb.AppendLine('class MDX_ParityPoses')
  [void]$sb.AppendLine('{')
  [void]$sb.AppendLine("`tstatic float Hour() { return $($job.HOUR); }")
  [void]$sb.AppendLine("`tstatic float Settle() { return $($job.SETTLE); }")
  [void]$sb.AppendLine("`tstatic float FirstSettle() { return $($job.FIRST); }")
  [void]$sb.AppendLine("`tstatic float FovY() { return $($job.FOVY); }")
  [void]$sb.AppendLine("`tstatic bool ExitWhenDone() { return true; }")
  [void]$sb.AppendLine("`tstatic void Fill(notnull array<ref MDX_ParityPose> poses)")
  [void]$sb.AppendLine("`t{")
  foreach ($p in $poses) {
    if ($p.Raw -ne '') { continue }
    $absText = if ($p.Abs) { 'true' } else { 'false' }
    [void]$sb.AppendLine("`t`tposes.Insert(new MDX_ParityPose(`"$($p.Name)`", $($p.X), $($p.Z), $($p.Alt), $absText, $($p.Az), $($p.El)));")
  }
  [void]$sb.AppendLine("`t}")
  [void]$sb.AppendLine('}')
  [IO.File]::WriteAllText((Join-Path $proj 'Scripts\Game\MDX\MDX_ParityPoses.c'), $sb.ToString())
  $shotDir = Join-Path $wbRoot 'profile\MDXParity'
  if (Test-Path $shotDir) { Remove-Item $shotDir -Recurse -Force }
  $exe = Join-Path $job.TOOLS 'Workbench\ArmaReforgerWorkbenchSteamDiag.exe'
  if (-not (Test-Path $exe)) { St "NO WORKBENCH: $exe"; exit 1 }
  $gproj = Join-Path $proj 'addon.gproj'
  St "workbench $($poses.Count) poses, project $gproj"
  $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe -Parent) -PassThru -ArgumentList @(
    '-gproj', "`"$gproj`"", '-addonsDir', "`"$($job.REFORGER)`"", '-wbModule=WorldEditor', '-run',
    '-plugin=MDX_ParityCapturePlugin')
  $null = $p.Handle
  $deadline = (Get-Date).AddSeconds([int]$job.WBTIMEOUT)
  while (-not $p.HasExited) {
    if (Test-Path (Join-Path $shotDir 'DONE')) { Start-Sleep 20; break }
    if (Test-Path (Join-Path $here 'STOP')) { St 'STOPPED'; break }
    if ((Get-Date) -gt $deadline) { St 'WORKBENCH TIMEOUT'; break }
    Start-Sleep 5
  }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  $out = Join-Path $here "shots\$tag-reforger"
  New-Item -ItemType Directory -Force $out | Out-Null
  if (Test-Path $shotDir) { Copy-Item (Join-Path $shotDir '*') $out -Recurse -Force }
  $wbLogs = Join-Path $wbRoot 'logs'
  $lastLog = Get-ChildItem $wbLogs -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($lastLog) { Copy-Item (Join-Path $lastLog.FullName '*.log') $out -ErrorAction SilentlyContinue }
  St "DONE workbench -> $out"
  exit 0
}

Wait-GameFree
# DEPLOY=1: unpack incoming\<ZIP> (default: the newest OpenPoseidon-Reforger-*.zip) into the EXE
# folder (default bin\) first. Builds of different branches go to different folders (EXE=bin-grass).
if ($job.DEPLOY -eq '1') {
  $zipFilter = if ($job.ZIP -ne '') { $job.ZIP } else { 'OpenPoseidon-Reforger-*.zip' }
  $zipItem = Get-ChildItem (Join-Path $here 'incoming') -Filter $zipFilter -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $zipItem) { St "NO ZIP (incoming\$zipFilter)"; exit 1 }
  $ex = Join-Path $here 'bin.new'
  Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
  St "deploy $($zipItem.FullName)"
  Expand-Archive $zipItem.FullName $ex -Force
  if (-not (Test-Path "$ex\OpenPoseidon.exe") -or -not (Test-Path "$ex\wgpu_renderer.dll")) { St 'ZIP INCOMPLETE'; exit 1 }
  $bin = Join-Path $here $job.EXE
  New-Item -ItemType Directory -Force $bin | Out-Null
  $t = Get-Date
  foreach ($f in Get-ChildItem $ex -File) { Copy-Item $f.FullName (Join-Path $bin $f.Name) -Force; (Get-Item (Join-Path $bin $f.Name)).LastWriteTime = $t }
  Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
  $built = if (Test-Path "$bin\BUILT-FROM.txt") { Get-Content "$bin\BUILT-FROM.txt" } else { @() }
  ($built + "installed from $($zipItem.FullName) by _OpenPoseidon-Reforger\go.ps1" + "deployed=$(Get-Date -Format 'dd.MM.yyyy HH:mm')") | Set-Content "$bin\DEPLOYED-FROM.txt"
  St "DEPLOYED $(($built | Select-String 'commit=') -join ' ')"
}
$exe = if ($job.EXE -eq 'game') { Join-Path $game 'OpenPoseidon.exe' } else { Join-Path (Join-Path $here $job.EXE) 'OpenPoseidon.exe' }
if (-not (Test-Path $exe)) { St "NO EXE: $exe"; exit 1 }
$built = Join-Path (Split-Path $exe -Parent) 'BUILT-FROM.txt'
$commit = if (Test-Path $built) { ((Get-Content $built | Select-String 'commit=') -join ' ') } else { 'unknown' }
St "exe $exe ($commit)"

$out = Join-Path $here "shots\$tag"
New-Item -ItemType Directory -Force $out | Out-Null
$user = Join-Path $here 'user'
New-Item -ItemType Directory -Force $user | Out-Null
$gfx = Join-Path $user 'graphics.cfg'
if (-not (Test-Path $gfx)) {
  # The benchmark Ultra profile (scripts/Write-BenchmarkGraphics.ps1): MSAA 4, A2C, no DLSS.
  $lines = @('version=3;', 'qualityPreset=3;', 'terrainDetail=4;', 'objectLod=4;', 'shadowQuality=4;', 'particlesQuality=4;',
    'grassQuality=4;', 'msaaSamples=4;', 'alphaToCoverage=1;', 'renderScale=1;', 'brightness=1.6;', 'gamma=1.2;',
    'dlssMode=0;', 'upscalerQuality=67;', 'vsync=0;', 'fpsCap=0;')
  [IO.File]::WriteAllText($gfx, (($lines -join "`n") + "`n"))
}

# The mission: perf_field's one soldier, with an init.sqs that flies the camera through the poses.
$mission = Join-Path $here 'run\parity.eden'
Remove-Item (Join-Path $here 'run') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $mission | Out-Null
Copy-Item (Join-Path $here 'mission\mission.sqm') $mission
$sqs = New-Object Text.StringBuilder
[void]$sqs.AppendLine('; generated by go.ps1 from poses.txt')
# --test-world-freefly installs its own camera a few hundred ms AFTER init.sqs starts; a pose set
# at t=0 is overwritten by it (every first pose until 2026-10-15 was taken from 400 m).
[void]$sqs.AppendLine('~3')
$first = $true
foreach ($p in $poses) {
  if ($p.Raw -ne '') { [void]$sqs.AppendLine($p.Raw); continue }
  $wait = if ($first) { $job.FIRST } else { $job.SETTLE }
  if ($p.Abs) {
    [void]$sqs.AppendLine("_r = triFreeFlyPose `"$($p.X) $($p.Z) $($p.Alt) $($p.Az) $($p.El)`"")
  } else {
    [void]$sqs.AppendLine("_h = triTerrainHeight [$($p.X), $($p.Z)]")
    [void]$sqs.AppendLine("_r = triFreeFlyPose format [`"%1 %2 %3 %4 %5`", $($p.X), $($p.Z), _h + $($p.Alt), $($p.Az), $($p.El)]")
  }
  [void]$sqs.AppendLine("logInfo format [`"parity pose $($p.Name): %1 ground %2`", _r, _h]")
  [void]$sqs.AppendLine("~$wait")
  [void]$sqs.AppendLine("_r = triScreenshotNext `"$($p.Name)`"")
  [void]$sqs.AppendLine("logInfo format [`"parity shot $($p.Name): %1`", _r]")
  [void]$sqs.AppendLine('~2')
  $first = $false
}
[void]$sqs.AppendLine('~2')
[void]$sqs.AppendLine('triEndGame')
[IO.File]::WriteAllText((Join-Path $mission 'init.sqs'), $sqs.ToString())

$env:POSEIDON_USER_DIR = $user
$env:POSEIDON_REFORGER_WORLD = 'worlds/eden'
$env:POSEIDON_REFORGER_OBJECTS = '1'
$env:POSEIDON_REFORGER_STREAM = '1'
$env:POSEIDON_VSYNC = '0'
$env:POSEIDON_TEST_OVERCAST = '0'
$env:POSEIDON_FREEFLY_FOV = $job.FOV
$env:TRI_OUTPUT_DIR = $out
foreach ($k in @($job.Keys)) { if ($k -like 'POSEIDON_*' -or $k -like 'WGR_*' -or $k -like 'OP_*') { Set-Item "env:$k" $job[$k] } }

$p0 = @($poses | Where-Object { $_.Raw -eq '' })[0]
$log = Join-Path $out 'run.log'
$err = Join-Path $out 'stderr.txt'
$al = @("--render=$($job.RENDER)", '--window', "--width=$($job.WIDTH)", "--height=$($job.HEIGHT)",
  '--test-mission', "`"$mission`"", '--test-world', "`"$($job.REFORGER)`"", '--reforger-world', 'worlds/eden',
  '--test-world-freefly', $p0.X, $p0.Z, '400', $p0.Az, $p0.El, '--test-world-hour', $job.HOUR,
  '--log-file', "`"$log`"", '--log-level', 'info')
if ($job.EXTRA -ne '') { $al += $job.EXTRA.Split(' ') }
($al -join ' ') | Set-Content (Join-Path $out 'args.txt')
$limit = [int]$job.FIRST + $poses.Count * ([int]$job.SETTLE + 4) + 900
St "launch $($poses.Count) poses, limit $limit s -> $out"
$sw = [Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $exe -ArgumentList $al -WorkingDirectory $game -PassThru -RedirectStandardError $err
$null = $proc.Handle
$timedOut = -not $proc.WaitForExit($limit * 1000)
if ($timedOut) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 2 }
$code = if ($timedOut) { 'TIMEOUT' } else { $proc.ExitCode }
if ($job.KEEPBMP -ne '1') { Get-ChildItem $out -Filter '*.bmp' | Remove-Item -ErrorAction SilentlyContinue }
"exit=$code seconds=$([int]$sw.Elapsed.TotalSeconds) commit=$commit" | Set-Content (Join-Path $out 'result.txt')
St "DONE exit $code after $([int]$sw.Elapsed.TotalSeconds) s -> $out"
} finally {
  Advance-Queue
}
