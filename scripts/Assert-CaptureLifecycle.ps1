param([Parameter(Mandatory)][string]$MetadataPath,
      [Parameter(Mandatory)][string]$LogPath)
$ErrorActionPreference = 'Stop'
$meta = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
foreach ($field in @('status', 'timed_out', 'contended', 'exit_code')) {
    if ($null -eq $meta -or $field -notin $meta.PSObject.Properties.Name -or $null -eq $meta.$field) {
        throw "Capture metadata is missing required field ${field}: $MetadataPath"
    }
}
if ($meta.timed_out -isnot [bool] -or $meta.contended -isnot [bool] -or
    ($meta.exit_code -isnot [int] -and $meta.exit_code -isnot [long])) {
    throw "Capture metadata contains invalid lifecycle field types: $MetadataPath"
}
if ($meta.status -ne 'ok' -or $meta.timed_out -or $meta.contended -or $meta.exit_code -ne 0) {
    throw "Capture lifecycle failed: status=$($meta.status), timeout=$($meta.timed_out), contention=$($meta.contended), exit=$($meta.exit_code): $MetadataPath"
}
if (!(Select-String -LiteralPath $LogPath -Pattern 'Shutdown complete' -Quiet)) {
    throw "Capture has no completed shutdown: $LogPath"
}
