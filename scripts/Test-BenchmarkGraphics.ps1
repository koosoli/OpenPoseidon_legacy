$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('poseidon-graphics-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$path = Join-Path $root 'graphics.cfg'
try {
    foreach ($mode in @(-1,0,1,2)) {
        & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $root -DlssMode $mode
        $before = [IO.File]::ReadAllText($path)
        foreach ($line in @('version=3;', 'fpsCap=0;', 'vsync=0;', 'renderScale=1;',
                'msaaSamples=4;', 'brightness=1.6;', 'gamma=1.2;', "dlssMode=$mode;")) {
            if (($before -split "`n") -notcontains $line) { throw "Missing explicit graphics setting: $line" }
        }
        $refused = $false
        try { & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $root -DlssMode $mode }
        catch { $refused = $true }
        if (!$refused -or [IO.File]::ReadAllText($path) -ne $before) { throw 'Existing profile was not protected' }
        Remove-Item -LiteralPath $path
    }
    $refused = $false
    try { & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir (Join-Path $root 'absent') }
    catch { $refused = $true }
    if (!$refused) { throw 'Missing isolated directory was accepted' }
    Write-Host 'Benchmark graphics: four modes, overwrite protection and missing-directory checks passed'
}
finally {
    # Only our known fixture file and empty unique directory, never recursive.
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    Remove-Item -LiteralPath $root
}
