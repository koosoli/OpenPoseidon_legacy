$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('poseidon-capture-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$metadata = Join-Path $root 'capture.json'
$log = Join-Path $root 'capture.log'
$cases = @(
    @{name='complete'; pass=$true; value=@{status='ok';timed_out=$false;contended=$false;exit_code=0}},
    @{name='failed'; pass=$false; value=@{status='failed';timed_out=$false;contended=$false;exit_code=0}},
    @{name='timeout'; pass=$false; value=@{status='ok';timed_out=$true;contended=$false;exit_code=0}},
    @{name='contention'; pass=$false; value=@{status='ok';timed_out=$false;contended=$true;exit_code=0}},
    @{name='exit'; pass=$false; value=@{status='ok';timed_out=$false;contended=$false;exit_code=1}},
    @{name='missing timeout'; pass=$false; value=@{status='ok';contended=$false;exit_code=0}},
    @{name='missing contention'; pass=$false; value=@{status='ok';timed_out=$false;exit_code=0}},
    @{name='null timeout'; pass=$false; value=@{status='ok';timed_out=$null;contended=$false;exit_code=0}},
    @{name='string timeout'; pass=$false; value=@{status='ok';timed_out='false';contended=$false;exit_code=0}},
    @{name='string exit'; pass=$false; value=@{status='ok';timed_out=$false;contended=$false;exit_code='0'}},
    @{name='no shutdown'; pass=$false; noShutdown=$true; value=@{status='ok';timed_out=$false;contended=$false;exit_code=0}}
)
try {
    foreach ($case in $cases) {
        [IO.File]::WriteAllText($metadata, ($case.value | ConvertTo-Json))
        [IO.File]::WriteAllText($log, $(if ($case.noShutdown) {'screenshot saved'} else {'Shutdown complete'}))
        $accepted = $true
        try { & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $metadata -LogPath $log }
        catch { $accepted = $false }
        if ($accepted -ne $case.pass) { throw "Lifecycle regression: $($case.name), accepted=$accepted" }
    }
    Write-Host "$($cases.Count) capture lifecycle cases passed"
}
finally {
    # Only the two known fixture files; no recursive cleanup or shared test path.
    Remove-Item -LiteralPath $metadata, $log -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -ErrorAction SilentlyContinue
}
