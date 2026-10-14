# Arma 3 test harness (OpenPoseidon --arma3): plays every mission of a list for N seconds.
# Lives in <CWA>\_OpenPoseidon-Arma3 on the test PC and is started by go.cmd (Explorer
# double-click). Settings in job.txt (KEY=value). Writes go.status, tests\<mission>.log,
# tests\<mission>_1st.png / _3rd.png (SHOTS=1) and tests\exitcodes.txt. A file named STOP
# next to this script (stop.cmd) ends the run after the current mission.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$game = 'H:\SteamLibrary\steamapps\common\ARMA Cold War Assault\Remastered'
$a3   = 'F:\SteamLibrary\steamapps\common\Arma 3'
$job = @{ WAIT='480'; QUIET='90'; DEPLOY='0'; LIST='missions_first.txt'; OUT='tests'; SECONDS='40'; RENDER=''; EXTRA=''; WINDOW='1'; SHOTS='1'; EXE='bin' }
if (Test-Path "$here\job.txt") {
  Get-Content "$here\job.txt" | ForEach-Object { if ($_ -match '^\s*([A-Z0-9_]+)\s*=(.*)$') { $job[$matches[1]] = $matches[2].Trim() } }
}
$status = "$here\go.status"
function St($s) { "$(Get-Date -Format 'HH:mm:ss') $s" | Add-Content $status }
Set-Content $status "start $(Get-Date -Format s)"
Remove-Item "$here\STOP" -ErrorAction SilentlyContinue
# One game at a time (CLAUDE.md rule 0d): another session's or the owner's OpenPoseidon is
# waited for, and the run starts only after no instance has been seen for QUIET seconds
# (another harness starts its next mission within seconds). WAIT=0 gives up at once.
function Wait-GameFree {
  $quiet = [int]$job.QUIET; $deadline = (Get-Date).AddMinutes([int]$job.WAIT); $free = $null; $said = $false
  while ($true) {
    if (Test-Path "$here\STOP") { St 'STOPPED while waiting'; exit 1 }
    if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {
      $free = $null
      if (-not $said) { St 'BUSY: OpenPoseidon laeuft schon, warte'; $said = $true }
      if ((Get-Date) -gt $deadline) { St 'BUSY: gave up waiting'; exit 1 }
    } elseif (-not $free) { $free = Get-Date }
    elseif (((Get-Date) - $free).TotalSeconds -ge $quiet) { if ($said) { St 'free' }; return }
    Start-Sleep 5
  }
}
Wait-GameFree
# DEPLOY=1: unpack the newest CI artifact zip (OpenPoseidon-Windows-MrDudeX-arma3-<sha>.zip,
# downloaded from the "Arma 3 / Windows game build" run) into bin\ first. Looked for in
# incoming\, next to this script and in the user's Downloads folder.
if ($job.DEPLOY -eq '1') {
  $dirs = @("$here\incoming", $here, (Join-Path $env:USERPROFILE 'Downloads')) | Where-Object { Test-Path $_ }
  $zipItem = $dirs | ForEach-Object { Get-ChildItem $_ -Filter 'OpenPoseidon-Windows-MrDudeX-arma3-*.zip' -ErrorAction SilentlyContinue } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $zipItem) { St 'NO ZIP (OpenPoseidon-Windows-MrDudeX-arma3-*.zip in incoming, here or Downloads)'; exit 1 }
  $ex = Join-Path $here 'bin.new'
  Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
  St "deploy $($zipItem.FullName)"
  Expand-Archive $zipItem.FullName $ex -Force
  if (-not (Test-Path "$ex\OpenPoseidon.exe") -or -not (Test-Path "$ex\wgpu_renderer.dll")) { St 'ZIP INCOMPLETE'; exit 1 }
  $bin = Join-Path $here 'bin'
  New-Item -ItemType Directory -Force $bin | Out-Null
  $t = Get-Date
  foreach ($f in Get-ChildItem $ex -File) { Copy-Item $f.FullName (Join-Path $bin $f.Name) -Force; (Get-Item (Join-Path $bin $f.Name)).LastWriteTime = $t }
  Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
  $built = if (Test-Path "$bin\BUILT-FROM.txt") { Get-Content "$bin\BUILT-FROM.txt" } else { @() }
  ($built + "installed from $($zipItem.FullName) by _OpenPoseidon-Arma3\go.ps1" + "deployed=$(Get-Date -Format 'dd.MM.yyyy HH:mm')") | Set-Content "$bin\DEPLOYED-FROM.txt"
  St "DEPLOYED $(($built | Select-String 'commit=') -join ' ')"
}
# The build under test: _OpenPoseidon-Arma3\bin (exe + its own wgpu_renderer.dll), run with the
# game folder as working directory, so the shared Remastered binaries are never touched.
# EXE=game runs the binaries installed in Remastered instead.
$exe = if ($job.EXE -eq 'game') { "$game\OpenPoseidon.exe" } else { Join-Path (Join-Path $here $job.EXE) 'OpenPoseidon.exe' }
if (-not (Test-Path $exe)) { St "NO EXE: $exe"; exit 1 }
St "exe $exe"
$out = Join-Path $here $job.OUT
New-Item -ItemType Directory -Force $out | Out-Null
$secs = [int]$job.SECONDS
foreach ($k in @($job.Keys)) { if ($k -like 'OP_*' -or $k -like 'WGPU_*' -or $k -like 'BMTR_*') { Set-Item "env:$k" $job[$k] } }
$missions = Get-Content (Join-Path $here $job.LIST) | Where-Object { $_.Trim() -ne '' -and -not $_.Trim().StartsWith('#') }
$i = 0
foreach ($m in $missions) {
  $i++
  if (Test-Path "$here\STOP") { St 'STOPPED'; break }
  if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { Wait-GameFree }
  $name = ($m.Trim() -replace '[\\/:]', '_')
  $log = Join-Path $out "$name.log"; $err = Join-Path $out "$name.stderr.txt"
  Remove-Item $log, $err -ErrorAction SilentlyContinue
  St "[$i/$($missions.Count)] $m"
  $al = @('--arma3', "`"$a3`"", '--arma3-mission', "`"$($m.Trim())`"", '--arma3-test-seconds', "$secs", '--log-file', "`"$log`"")
  if ($job.WINDOW -eq '1') { $al += '--window' }
  if ($job.RENDER -ne '') { $al += @('--render', $job.RENDER) }
  if ($job.EXTRA -ne '') { $al += $job.EXTRA.Split(' ') }
  if ($job.SHOTS -eq '1') { $env:OP_A2_SHOT = Join-Path $out $name } else { Remove-Item env:OP_A2_SHOT -ErrorAction SilentlyContinue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $al -WorkingDirectory $game -PassThru -RedirectStandardError $err
  $null = $p.Handle
  $limit = $secs + 300
  $timedOut = -not $p.WaitForExit($limit * 1000)
  if ($timedOut) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 2 }
  $code = if ($timedOut) { 'TIMEOUT' } else { $p.ExitCode }
  "$m`t$code`t$([int]$sw.Elapsed.TotalSeconds)" | Add-Content (Join-Path $out 'exitcodes.txt')
  Start-Sleep 2
}
St 'DONE'
