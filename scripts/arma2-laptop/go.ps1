# Laptop test harness: deploy a CI build (optional), then play each mission of a list.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$game = 'C:\Program Files (x86)\Steam\steamapps\common\ARMA Cold War Assault\Remastered'
$a2   = 'C:\Program Files (x86)\Steam\steamapps\common\Arma 2'
$job = @{ DEPLOY='0'; LIST='missions_all.txt'; OUT='tests'; SECONDS='40'; RENDER='gl33'; EVAL=''; PERIOD=''; EXTRA='' }
Get-Content "$here\job.txt" | ForEach-Object { if ($_ -match '^\s*([A-Z0-9_]+)\s*=(.*)$') { $job[$matches[1]] = $matches[2].Trim() } }
$status = "$here\go.status"
function St($s) { "$(Get-Date -Format 'HH:mm:ss') $s" | Add-Content $status }
Set-Content $status "start $(Get-Date -Format s)"
Remove-Item "$here\STOP" -ErrorAction SilentlyContinue
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { St 'BUSY: OpenPoseidon laeuft schon'; exit 1 }

if ($job.DEPLOY -eq '1') {
  # newest CI artifact zip the owner downloaded into Downloads
  $zipItem = Get-ChildItem "$here\.." -Filter 'OpenPoseidon-Windows-*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $zipItem) { St 'NO ZIP in Downloads'; exit 1 }
  $zip = $zipItem.FullName; $ex = "$here\build"
  Remove-Item $ex -Recurse -Force -ErrorAction SilentlyContinue
  St "deploy $($zipItem.Name)"
  Expand-Archive $zip $ex -Force
  if (-not (Test-Path "$ex\OpenPoseidon.exe") -or -not (Test-Path "$ex\wgpu_renderer.dll")) { St 'ZIP INCOMPLETE'; exit 1 }
  $t = Get-Date
  foreach ($f in 'OpenPoseidon.exe','wgpu_renderer.dll','OpenAL32.dll','OpenPoseidon.pdb','wgpu_renderer.pdb') {
    if (Test-Path "$ex\$f") { Copy-Item "$ex\$f" "$game\$f" -Force; (Get-Item "$game\$f").LastWriteTime = $t }
  }
  ((Get-Content "$ex\BUILT-FROM.txt") + "installed from Downloads\$($zipItem.Name) by a2test\go.ps1" + "deployed=$(Get-Date -Format 'dd.MM.yyyy HH:mm')") | Set-Content "$game\DEPLOYED-FROM.txt"
  St 'DEPLOYED'
}

$out = Join-Path $here $job.OUT
New-Item -ItemType Directory -Force $out | Out-Null
$secs = [int]$job.SECONDS
$env:OP_A2_EVAL = $job.EVAL; $env:OP_A2_PERIOD = $job.PERIOD
foreach ($k in @($job.Keys)) { if ($k -like 'OP_*' -or $k -like 'WGPU_*') { Set-Item "env:$k" $job[$k] } }
$missions = Get-Content (Join-Path $here $job.LIST) | Where-Object { $_.Trim() -ne '' }
$i = 0
foreach ($m in $missions) {
  $i++
  if (Test-Path "$here\STOP") { St 'STOPPED'; break }
  $name = ($m.Trim() -replace '[\\/]', '_')
  $log = Join-Path $out "$name.log"; $err = Join-Path $out "$name.stderr.txt"
  Remove-Item $log, $err -ErrorAction SilentlyContinue
  St "[$i/$($missions.Count)] $m"
  $al = @('--arma2', "`"$a2`"", '--arma2-mission', "`"$($m.Trim())`"", '--arma2-test-seconds', "$secs", '--log-file', "`"$log`"")
  if ($job.RENDER -ne '') { $al += @('--render', $job.RENDER) }
  if ($job.EXTRA -ne '') { $al += $job.EXTRA.Split(' ') }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath "$game\OpenPoseidon.exe" -ArgumentList $al -WorkingDirectory $game -PassThru -RedirectStandardError $err
  $null = $p.Handle
  $limit = $secs + 240
  $timedOut = -not $p.WaitForExit($limit * 1000)
  if ($timedOut) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 2 }
  $code = if ($timedOut) { 'TIMEOUT' } else { $p.ExitCode }
  "$m`t$code`t$([int]$sw.Elapsed.TotalSeconds)" | Add-Content (Join-Path $out 'exitcodes.txt')
  Start-Sleep 2
}
St 'DONE'
