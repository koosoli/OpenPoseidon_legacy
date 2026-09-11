$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('poseidon-dump-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$path = Join-Path $root 'fixture.dmp'
$exe = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$child = Start-Process -FilePath $exe -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 60"' -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 500
    $refused = $false
    try { & "$PSScriptRoot/Write-OwnedProcessMinidump.ps1" -Process $child -ExpectedExe (Join-Path $root 'wrong.exe') -Path $path }
    catch { $refused = $true }
    if (!$refused -or (Test-Path -LiteralPath $path)) { throw 'Mismatched executable was not refused' }
    & "$PSScriptRoot/Write-OwnedProcessMinidump.ps1" -Process $child -ExpectedExe $exe -Path $path
    $stream = [IO.File]::OpenRead($path)
    try {
        $magic = New-Object byte[] 4
        if ($stream.Read($magic,0,4) -ne 4 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'MDMP') {
            throw 'Not a Windows minidump'
        }
    } finally { $stream.Dispose() }
    if ($child.HasExited) { throw 'Dump unexpectedly terminated the fixture' }
    $length = (Get-Item -LiteralPath $path).Length
    $refused = $false
    try { & "$PSScriptRoot/Write-OwnedProcessMinidump.ps1" -Process $child -ExpectedExe $exe -Path $path }
    catch { $refused = $true }
    if (!$refused -or (Get-Item -LiteralPath $path).Length -ne $length) { throw 'Existing dump was not preserved' }
    Write-Output "Minidump ownership, MDMP signature and overwrite checks passed ($length bytes)"
} finally {
    if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }
    $child.Dispose()
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    Remove-Item -LiteralPath $root
}
