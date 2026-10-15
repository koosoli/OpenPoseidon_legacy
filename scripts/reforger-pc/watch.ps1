# Reforger parity watcher: started once (watch.cmd), then runs go.ps1 whenever request.txt appears
# next to it, so a session that cannot click on the PC can still start runs by writing that file.
# request.txt holds KEY=value lines like job.txt (they override it). While a request runs it is
# request.running; afterwards request.done (with the end time). A file named STOP-WATCH ends the
# watcher; watch.status says what it is doing.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$status = Join-Path $here 'watch.status'
function Ws($s) { "$(Get-Date -Format 'HH:mm:ss') $s" | Add-Content $status }
Set-Content $status "watcher start $(Get-Date -Format s) pid $PID"
Remove-Item (Join-Path $here 'STOP-WATCH') -ErrorAction SilentlyContinue
while (-not (Test-Path (Join-Path $here 'STOP-WATCH'))) {
  $req = Join-Path $here 'request.txt'
  if (Test-Path $req) {
    $running = Join-Path $here 'request.running'
    Move-Item $req $running -Force
    $merged = @()
    if (Test-Path (Join-Path $here 'job.txt')) { $merged += Get-Content (Join-Path $here 'job.txt') }
    $merged += Get-Content $running
    $merged | Set-Content (Join-Path $here 'request.job')
    Ws "run $((Get-Content $running) -join ' | ')"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'go.ps1') -JobFile (Join-Path $here 'request.job')
    Ws "done exit $LASTEXITCODE"
    ((Get-Content $running) + "finished=$(Get-Date -Format s)") | Set-Content (Join-Path $here 'request.done')
    Remove-Item $running -ErrorAction SilentlyContinue
  }
  Start-Sleep 5
}
Ws 'watcher stopped'
