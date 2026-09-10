param([Parameter(Mandatory)][string]$MetadataPath,
      [Parameter(Mandatory)][string]$LogPath)
$ErrorActionPreference = 'Stop'
$meta = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
if ($meta.status -ne 'ok' -or $meta.timed_out -or $meta.contended -or $meta.exit_code -ne 0) {
    throw "Capture lifecycle failed: status=$($meta.status), timeout=$($meta.timed_out), contention=$($meta.contended), exit=$($meta.exit_code): $MetadataPath"
}
if (!(Select-String -LiteralPath $LogPath -Pattern 'Shutdown complete' -Quiet)) {
    throw "Capture has no completed shutdown: $LogPath"
}
