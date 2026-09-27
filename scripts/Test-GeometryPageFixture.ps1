param(
    [switch]$Disabled,
    [switch]$Clod,
    [string]$Label='geometry-page-fixture',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Python='C:\Program Files\Python311\python.exe'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
    $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
    $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),
        '-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
    if ($Disabled) {$argv+='-Disabled'}
    if ($Clod) {$argv+='-Clod'}
    $env:LOCK_OWNER='private resident geometry page fixture'
    try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Geometry fixture exited $LASTEXITCODE"}}
    finally {Remove-Item Env:LOCK_OWNER}
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$fixture=Join-Path $out 'fixture'; $mission=Join-Path $out 'geometry-fixture.eden'
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $mission,$env:POSEIDON_USER_DIR | Out-Null
& $Python (Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py') $fixture --absolute-model-path
if ($LASTEXITCODE) {throw 'Original world fixture generation failed'}
@'
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=12; minute=0; };
 class Groups { items=1; class Item0 { side="WEST"; class Vehicles { items=1; class Item0 {
  position[]={100,0,100}; id=0; side="WEST"; vehicle="SoldierWB";
  player="PLAYER COMMANDER"; leader=1; skill=1;
 }; }; }; };
};
class Intro { randomSeed=1; class Intel {}; };
class OutroWin { randomSeed=2; class Intel {}; };
class OutroLoose { randomSeed=3; class Intel {}; };
'@ | Set-Content -LiteralPath (Join-Path $mission 'mission.sqm')
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:WGR_GEOMETRY_PAGE_FIXTURE=if ($Disabled) {'0'} else {'1'}
foreach ($key in @('WGR_GEOMETRY_OWNER_LEDGER','WGR_SIMULATION_RESIDENCY','WGR_SIMULATION_RESIDENCY_TEST',
    'WGR_SIMULATION_POSITIVE_COLD','WGR_OBJECT_STREAM_GPU_BUDGET','WGR_WATER_CURLING_BREAKER',
    'WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS','WGR_OBJECT_STREAM_WARM_TEXTURES','WGR_OBJECT_STREAM_WARM_PROFILE',
    'WGR_OBJECT_STREAM_REGISTRATION_QUOTA','WGR_GEOMETRY_MESH_ACK','WGR_GEOMETRY_REGISTERED_REFS',
    'WGR_GEOMETRY_LOD_FEEDBACK','WGR_OBJECT_STREAM_WINDOW_GROWTH','WGR_NATIVE_DDS_PREPARE',
    'WGR_NATIVE_DDS_BC3_ONLY','WGR_ADAPTIVE_TEXTURE_DETAIL','WGR_PAA_LZO_REPLAY_CACHE',
    'WGR_OBJECT_STREAM_PBO','WGR_OBJECT_STREAM_PBO_TEXTURES')) {Set-Item ('Env:'+ $key) '0'}
foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN')) {
    Remove-Item ('Env:'+ $key) -ErrorAction SilentlyContinue
}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$log=Join-Path $out 'engine.log'
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));
    disabled=[bool]$Disabled;clod=[bool]$Clod;scriptSha256=(Get-FileHash $PSCommandPath).Hash;
    hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
    $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @(
        '--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",
        '--test-world-freefly','1184','1240','18','90','-8','--test-world-hour','12',
        '--test-mission',('"'+$mission+'"'),'--test-world',('"'+(Join-Path $fixture 'simulation-residency.wrp')+'"'),
        '--log-file',('"'+$log+'"'))
    $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
    while (!$client.Connected) {try {$client.Connect('127.0.0.1',$port)} catch {
        if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw};Start-Sleep -Milliseconds 250
    }}
    $stream=$client.GetStream();$stream.ReadTimeout=30000
    $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Harness closed'};$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
        ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl')
        $line | Add-Content (Join-Path $out 'harness.jsonl')
        if (!$reply.ok) {throw $line};return $reply
    }
    function Action($action) {Send @{cmd='geometry_page_fixture';action=$action;x=1200;y=15;z=1240}}
    function Capture($name) {
        $path=Join-Path $out $name
        $null=Send @{cmd='screenshot';path=$path}
        $until=[DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath $path) -and [DateTime]::UtcNow -lt $until) {Start-Sleep -Milliseconds 100}
        if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) {throw "Missing capture: $path"}
    }
    function Observe($active,$fine) {
        $until=[DateTime]::UtcNow.AddSeconds(15)
        do {
            $request=Action 'observe'
            do {Start-Sleep -Milliseconds 100;$cut=Action 'status'} while ($cut.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
            if ($cut.status -ne 'Ready') {throw "Fixture refused: $($cut | ConvertTo-Json -Compress)"}
            foreach ($field in @('requestId','observedRequestId','active','fineSelected','mainFrameReturned',
                'cpuHelpersUnchanged','helperHashBefore','helperHashAfter','sourceSha256','coarseTriangles','fineTriangles',
                'knownPayloadBytes','meshCount','meshesPresent','meshesAbsent','producerModel','rendererModel','producerInstance','rendererInstance')) {
                if ($null -eq $cut.$field) {throw "Missing fixture field: $field"}
            }
            if ($Clod) {
                foreach ($field in @('clodPilot','fallbackSelected','clodAdapterVersion','clodLibraryRevision',
                    'authoredFineTriangles','fallbackTriangles','clodGroups','clodClusters',
                    'coarseThreshold','fineThreshold','clodBakeMs')) {
                    if ($null -eq $cut.$field) {throw "Missing CLOD fixture field: $field"}
                }
                foreach ($field in @('coarseThreshold','fineThreshold','clodBakeMs')) {
                    if ([double]::IsNaN([double]$cut.$field) -or [double]::IsInfinity([double]$cut.$field)) {
                        throw "Nonfinite CLOD fixture field: $field"
                    }
                }
            }
            if (!$active -and $cut.meshesPresent -gt 0 -and [DateTime]::UtcNow -lt $until) {continue}
            if ($null -eq $request.requestId -or $request.requestId -lt 1 -or $cut.requestId -lt 1 -or
                $cut.observedRequestId -ne $request.requestId -or $cut.active -ne $active -or
                $cut.fineSelected -ne $fine -or !$cut.mainFrameReturned -or !$cut.cpuHelpersUnchanged -or
                !$cut.helperHashBefore -or !$cut.helperHashAfter -or $cut.sourceSha256 -notmatch '^[a-fA-F0-9]{64}$' -or
                $cut.helperHashBefore -ne $cut.helperHashAfter -or
                (!$Clod -and ($cut.coarseTriangles -ne 12 -or $cut.fineTriangles -ne 48)) -or
                ($Clod -and (!$cut.clodPilot -or $cut.clodAdapterVersion -ne 1 -or
                    $cut.clodLibraryRevision -ne '9e1f07b159d3cb777f1c67ed31fc11fd117986f4' -or
                    $cut.authoredFineTriangles -ne 512 -or $cut.fallbackTriangles -ne 2 -or
                    $cut.coarseTriangles -le 0 -or $cut.fineTriangles -le $cut.coarseTriangles -or
                    $cut.fineTriangles -gt 512 -or $cut.clodGroups -lt 2 -or $cut.clodClusters -lt 1 -or $cut.clodClusters -gt 64 -or
                    $cut.coarseThreshold -le 0 -or $cut.fineThreshold -ne 0 -or $cut.clodBakeMs -lt 0)) -or
                $cut.knownPayloadBytes -le 0 -or $cut.knownPayloadBytes -gt 1048576 -or $cut.meshCount -lt 1 -or $cut.meshCount -gt 64 -or
                $cut.meshesPresent -lt 0 -or $cut.meshesAbsent -lt 0 -or ($cut.meshesPresent+$cut.meshesAbsent) -ne $cut.meshCount -or
                ($active -and ($cut.rendererModel -eq 4294967295 -or $cut.rendererInstance -eq 4294967295 -or $cut.meshesPresent -ne $cut.meshCount)) -or
                (!$active -and ($cut.rendererInstance -ne 4294967295 -or $cut.meshesAbsent -ne $cut.meshCount))) {
                throw "Unexpected resident cut: $($cut | ConvertTo-Json -Compress)"
            }
            return $cut
        } while ([DateTime]::UtcNow -lt $until)
        throw 'Private geometry records did not settle before deadline'
    }
    $begin=Action $(if($Clod){'beginClod'}else{'begin'})
    if ($Disabled) {
        foreach ($field in @('status','active','meshCount','knownPayloadBytes')) {if ($null -eq $begin.$field) {throw "Missing disabled fixture field: $field"}}
        if ($begin.status -ne 'Disabled' -or $begin.active -or $begin.meshCount -or $begin.knownPayloadBytes) {throw 'Default-off fixture unexpectedly allocated'}
        $result=@{begin=$begin;disabled=$true}
    } else {
        $fallback=$null; $fallbackReturned=$null
        if ($Clod) {
            $fallback=Observe $true $false
            if (!$fallback.fallbackSelected) {throw 'Clod pilot did not preserve authored fallback'}
            Capture 'fallback.png'
            $null=Action 'coarse'
        }
        $coarse=Observe $true $false
        if ($Clod -and ($coarse.fallbackSelected -or $coarse.rendererModel -eq $fallback.rendererModel -or
            $coarse.rendererInstance -ne $fallback.rendererInstance -or $coarse.producerInstance -ne $fallback.producerInstance)) {
            throw 'Coarse CLOD cut did not independently replace the authored fallback on the fixed instance'
        }
        Capture 'coarse.png'
        $null=Action 'fine';$fine=Observe $true $true
        if ($fine.rendererModel -eq $coarse.rendererModel -or $fine.rendererInstance -ne $coarse.rendererInstance -or
            $fine.producerInstance -ne $coarse.producerInstance) {throw 'Frontier did not switch one instance to the alternate model'}
        if ($Clod -and ($fine.fallbackSelected -or $fine.rendererModel -eq $fallback.rendererModel -or
            $fine.rendererInstance -ne $fallback.rendererInstance -or $fine.producerInstance -ne $fallback.producerInstance)) {
            throw 'Fine CLOD cut did not select the third independent model on the fixed instance'
        }
        Capture 'fine.png'
        $null=Action 'coarse';$returned=Observe $true $false
        if ($returned.rendererModel -ne $coarse.rendererModel -or $returned.rendererInstance -ne $coarse.rendererInstance -or
            $returned.producerInstance -ne $coarse.producerInstance) {throw 'Coarse model or fixed instance was not preserved'}
        if ($Clod) {
            if ($returned.fallbackSelected) {throw 'Returned coarse CLOD cut incorrectly selected authored fallback'}
            $null=Action 'fallback';$fallbackReturned=Observe $true $false
            if (!$fallbackReturned.fallbackSelected -or $fallbackReturned.rendererModel -ne $fallback.rendererModel -or
                $fallbackReturned.rendererInstance -ne $fallback.rendererInstance -or
                $fallbackReturned.producerInstance -ne $fallback.producerInstance) {throw 'Authored fallback was not independently retained'}
        }
        $null=Action 'abort';$retired=Observe $false $false
        $result=@{begin=$begin;fallback=$fallback;fallbackReturned=$fallbackReturned;coarse=$coarse;fine=$fine;returned=$returned;retired=$retired;disabled=$false;clod=[bool]$Clod}
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
    if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
    $result.passed=$true;$result.cleanExit=$true;$result.scope='Private original authored resident geometry, record lifetime and fixed-instance switching; no CLOD runtime paging, all-pass, device-free or performance acceptance'
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $out 'result.json')
    Write-Output "Geometry page fixture evidence: $out"
} catch {
    @{passed=$false;error=$_.Exception.Message;ownPid=if($p){$p.Id}else{$null};cleanupRequired=[bool]($p -and !$p.HasExited)} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'failure.json')
    throw
} finally {
    if ($client) {$client.Dispose()}
    if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
