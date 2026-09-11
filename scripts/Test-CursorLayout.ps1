param([int]$Width=1280, [int]$Height=720,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($Width -lt 640 -or $Height -lt 480) {throw 'Invalid test viewport'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/cursor-layout/'+(Get-Date -Format 'yyyyMMdd-HHmmss')+"-${Width}x$Height")
$user=Join-Path $out 'user'
New-Item -ItemType Directory -Force $user | Out-Null
Get-Content -LiteralPath "$GameDir/DEPLOYED-FROM.txt" | Set-Content -LiteralPath "$out/DEPLOYED-FROM.txt"
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
$env:POSEIDON_USER_DIR=$user
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$log=Join-Path $out 'cursor.log'
$p=Start-Process -FilePath "$GameDir/OpenPoseidon.exe" -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width',"$Width",'--height',"$Height",'--harness',"$port",'--log-file',('"'+$log+'"'))
$null=$p.Handle
$client=[Net.Sockets.TcpClient]::new()
try {
    $deadline=[DateTime]::UtcNow.AddSeconds(90)
    while (!$client.Connected) {
        try {$client.Connect('127.0.0.1',$port)} catch {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw}
            Start-Sleep -Milliseconds 250
        }
    }
    $stream=$client.GetStream(); $stream.ReadTimeout=30000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    function Send($q) {
        $writer.WriteLine(($q | ConvertTo-Json -Compress))
        do {$line=$reader.ReadLine(); if (!$line) {throw 'Harness disconnected'}; $r=$line | ConvertFrom-Json} while ($null -eq $r.ok)
        $line | Add-Content -LiteralPath "$out/responses.jsonl"
        if (!$r.ok) {throw $line}
        return $r
    }
    function CaptureCursor($name) {
        $null=Send @{cmd='exec';code='triCursorMove [0,0]'}
        $cursorDeadline=[DateTime]::UtcNow.AddSeconds(20)
        do {
            Start-Sleep -Seconds 1
            $draw=Send @{cmd='eval';code='triCursorDrawRect'}
        } while ($draw.result.Trim('"') -eq 'none' -and [DateTime]::UtcNow -lt $cursorDeadline)
        $null=Send @{cmd='screenshot';path="$out/$name.png"}
        if ($draw.result.Trim('"') -eq 'none') {throw "No cursor draw: inspect $out/$name.png"}
        $rect=($draw.result.Trim('"') -split ',') | ForEach-Object {[int]$_}
        if ($rect.Count -ne 5 -or $rect[3] -le 0 -or $rect[3] -ne $rect[4]) {throw "Non-square cursor: $($draw.result)"}
        Add-Type -AssemblyName System.Drawing
        # Harness acknowledges capture scheduling before the PNG is written.
        $capture=$null
        $captureDeadline=[DateTime]::UtcNow.AddSeconds(15)
        do {
            try {$capture=[Drawing.Image]::FromFile("$out/$name.png")} catch {
                if ([DateTime]::UtcNow -ge $captureDeadline) {throw}
                Start-Sleep -Milliseconds 200
            }
        } while (!$capture)
        try {
            if ($capture.Width -ne $Width -or $capture.Height -ne $Height) {
                throw "Unexpected viewport $($capture.Width)x$($capture.Height), requested ${Width}x$Height"
            }
        } finally {$capture.Dispose()}
        Write-Output "$name cursor: $($draw.result)"
    }
    Start-Sleep -Seconds 12
    CaptureCursor 'menu'
    $null=Send @{cmd='exec';code='triSetLanguage "English"; triClick 115'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='exec';code='triClick 1'}
    Start-Sleep -Seconds 8
    $display=Send @{cmd='eval';code='triDisplay'}
    if ($display.result -ne '26') {throw "Editor not open: $($display.result)"}
    CaptureCursor 'editor'
    $null=Send @{cmd='exec';code='triOpenDisabledChildDisplay 99001'}
    CaptureCursor 'child'
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Cursor test exit failed'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer failure'}
    Write-Output "Cursor layout runtime passed: $out"
} finally {
    $client.Dispose()
    if (!$p.HasExited) {$p.Kill(); $p.WaitForExit()}
    $p.Dispose()
}
