# Arma 3 test watcher: started once (watch.cmd), then runs a test whenever request.txt
# appears next to it, so a session that cannot click on the PC can still start runs by
# writing that file. request.txt holds KEY=value lines like job.txt (they override it).
# While a request runs it is request.running; afterwards request.done (with the end time).
# A file named STOP-WATCH ends the watcher; watch.status says what it is doing.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$status = "$here\watch.status"
function Ws($s) { "$(Get-Date -Format 'HH:mm:ss') $s" | Add-Content $status }
Set-Content $status "watcher start $(Get-Date -Format s) pid $PID"
Remove-Item "$here\STOP-WATCH" -ErrorAction SilentlyContinue
while (-not (Test-Path "$here\STOP-WATCH")) {
  if (Test-Path "$here\request.txt") {
    Move-Item "$here\request.txt" "$here\request.running" -Force
    $merged = @()
    if (Test-Path "$here\job.txt") { $merged += Get-Content "$here\job.txt" }
    $merged += Get-Content "$here\request.running"
    $merged | Set-Content "$here\request.job"
    Ws "run $((Get-Content "$here\request.running") -join ' | ')"
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$here\go.ps1" -JobFile "$here\request.job"
    Ws "done exit $LASTEXITCODE"
    ((Get-Content "$here\request.running") + "finished=$(Get-Date -Format s)") | Set-Content "$here\request.done"
    Remove-Item "$here\request.running" -ErrorAction SilentlyContinue
  }
  Start-Sleep 5
}
Ws 'watcher stopped'
