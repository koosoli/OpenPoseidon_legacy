param(
    [switch]$Disabled,
    [switch]$Clod,
    [switch]$Paged,
    [switch]$Evict,
    [string]$Label='geometry-page-fixture',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$Python='C:\Program Files\Python311\python.exe'
)
$ErrorActionPreference='Stop'
if ($Evict -and !$Paged) {throw '-Evict requires -Paged'}
if ($Paged -and $Clod -and !$Evict) {throw '-Clod -Paged requires -Evict for the bounded combined one-cycle proof'}
if (!$env:LOCK_OWNER) {
    $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
    $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),
        '-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
    if ($Disabled) {$argv+='-Disabled'}
    if ($Clod) {$argv+='-Clod'}
    if ($Paged) {$argv+='-Paged'}
    if ($Evict) {$argv+='-Evict'}
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
    disabled=[bool]$Disabled;clod=[bool]$Clod;paged=[bool]$Paged;evict=[bool]$Evict;scriptSha256=(Get-FileHash $PSCommandPath).Hash;
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
    $script:fixtureObservedFrame=0
    function Observe($active,$fine) {
        $until=[DateTime]::UtcNow.AddSeconds(15)
        do {
            $request=Action 'observe'
            do {Start-Sleep -Milliseconds 100;$cut=Action 'status'} while ($cut.status -eq 'Pending' -and
                !($Evict -and $cut.fineEvictionPending -and $cut.mainFrameReturned) -and [DateTime]::UtcNow -lt $until)
            if ($Evict -and $cut.status -eq 'Pending' -and $cut.fineEvictionPending -and
                $cut.mainFrameReturned -and [DateTime]::UtcNow -lt $until) {continue}
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
            $historyValid=$false
            if ($Evict) {
                foreach ($field in @('expectedPresentMeshes','expectedAbsentMeshes','observedExpectedPresentMeshes',
                    'observedExpectedAbsentMeshes','fineGeneration','fineEvictionCount','fineEvictionPending',
                    'retiredFineFirst','retiredFineCount','rendererMeshHandles')) {
                    if ($null -eq $cut.$field) {throw "Missing eviction history field: $field"}
                }
                $handles=@($cut.rendererMeshHandles)
                if ($handles.Count -ne $cut.meshCount -or $cut.fineEvictionCount -gt 1 -or $cut.fineGeneration -gt 2) {
                    throw 'Unbounded or missing full-generation mesh history'
                }
                foreach ($handle in $handles) {if ($handle -notmatch '^[1-9][0-9]*$') {throw 'Invalid historical renderer handle'}}
                if (@($handles | Select-Object -Unique).Count -ne $handles.Count) {throw 'Historical full-generation handles alias'}
                $historyValid=($cut.expectedPresentMeshes+$cut.expectedAbsentMeshes) -eq $cut.meshCount -and
                    $cut.observedExpectedPresentMeshes -eq $cut.expectedPresentMeshes -and
                    $cut.observedExpectedAbsentMeshes -eq $cut.expectedAbsentMeshes -and
                    $cut.meshesPresent -eq $cut.expectedPresentMeshes -and $cut.meshesAbsent -eq $cut.expectedAbsentMeshes
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
                ($active -and ($cut.rendererModel -eq 4294967295 -or $cut.rendererInstance -eq 4294967295 -or ((!$Evict -and $cut.meshesPresent -ne $cut.meshCount) -or ($Evict -and !$historyValid)))) -or
                (!$active -and ($cut.rendererInstance -ne 4294967295 -or $cut.meshesAbsent -ne $cut.meshCount))) {
                throw "Unexpected resident cut: $($cut | ConvertTo-Json -Compress)"
            }
            if ($null -eq $cut.renderReturnStatus -or $cut.renderReturnStatus -ne 0 -or $null -eq $cut.passFacts) {
                throw 'Real renderer frame/getter evidence missing or failed'
            }
            $pass=$cut.passFacts
            foreach ($field in @('valid','version','structBytes','enabled','frame','instanceEpoch','required','capabilities','pending',
                'recordedThisFrame','cascadeCount','cascadeDrawMask','cascadeEpoch','cascadeFrame','localCount','localDrawMask',
                'localValidMask','interiorDrawMask','interiorValidMask','giRsmEpoch','giRsmFrame','reflectionEpoch','reflectionFrame')) {
                if ($null -eq $pass.$field) {throw "Missing pass fact: $field"}
            }
            foreach ($field in @('frame','instanceEpoch','cascadeEpoch','cascadeFrame','giRsmEpoch','giRsmFrame','reflectionEpoch','reflectionFrame')) {
                $number=[double]$pass.$field
                if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or
                    $number -gt 9007199254740991 -or [Math]::Floor($number) -ne $number) {throw "Inexact pass fact: $field"}
            }
            foreach ($entry in @(@{name='localEpochs';count=24},@{name='localFrames';count=24},
                @{name='interiorEpochs';count=5},@{name='interiorFrames';count=5})) {
                if ($null -eq $pass.($entry.name) -or @($pass.($entry.name)).Count -ne $entry.count) {throw "Missing bounded array: $($entry.name)"}
                foreach ($element in @($pass.($entry.name))) {
                    if ($null -eq $element) {throw "Null bounded epoch/frame element: $($entry.name)"}
                    $number=[double]$element
                    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or
                        $number -gt 9007199254740991 -or [Math]::Floor($number) -ne $number) {throw "Inexact bounded epoch/frame element: $($entry.name)"}
                }
            }
            if (!$pass.valid -or $pass.version -ne 2 -or $pass.structBytes -ne 592 -or $pass.enabled -ne 1 -or
                $pass.frame -le $script:fixtureObservedFrame -or $pass.cascadeCount -gt 4 -or $pass.localCount -gt 24) {
                throw 'Invalid or nonmonotonic copied renderer pass facts'
            }
            if ($cut.cascadeConfigured -and (($pass.required -band 1) -eq 0 -or ($pass.capabilities -band 1) -eq 0 -or
                ($pass.pending -band 1) -ne 0 -or $pass.cascadeCount -lt 1 -or
                $pass.cascadeDrawMask -ne ((1 -shl [int]$pass.cascadeCount)-1) -or
                $pass.cascadeEpoch -ne $pass.instanceEpoch -or $pass.cascadeFrame -ne $pass.frame)) {
                throw 'Configured cascade lacks actual current-epoch command-recording facts'
            }
            if ($pass.capabilities -ne 31) {throw 'Unexpected pass recorder capabilities'}
            if (($pass.required -band 16) -ne 0 -and ($pass.pending -band 16) -eq 0 -and
                (($pass.recordedThisFrame -band 16) -eq 0 -or $pass.reflectionEpoch -ne $pass.instanceEpoch -or
                 $pass.reflectionFrame -ne $pass.frame)) {
                throw 'Reflection marked complete without actual same-frame command-recording facts'
            }
            # Configured reflection resource refusal and stale cached local/interior/GI may remain
            # Pending. These scoped facts do not claim visible fixture pixels or all-pass completeness.
            $script:fixtureObservedFrame=[double]$pass.frame
            return $cut
        } while ([DateTime]::UtcNow -lt $until)
        throw 'Private geometry records did not settle before deadline'
    }
    $beginAction=if($Paged -and $Clod){'beginClodPaged'}elseif($Paged){'beginPaged'}elseif($Clod){'beginClod'}else{'begin'}
    $begin=Action $beginAction
    if ($Disabled) {
        foreach ($field in @('status','active','meshCount','knownPayloadBytes')) {if ($null -eq $begin.$field) {throw "Missing disabled fixture field: $field"}}
        if ($begin.status -ne 'Disabled' -or $begin.active -or $begin.meshCount -or $begin.knownPayloadBytes) {throw 'Default-off fixture unexpectedly allocated'}
        $result=@{begin=$begin;disabled=$true}
    } elseif ($Paged) {
        function PageCut($cut) {
            foreach ($field in @('pagedPilot','fineResident','pageEpoch','pageRequest','pageWorkState',
                'pageQueued','pageActive','pageReady','pageLiveJobs','pageReservedBytes','pageCompleted','pageCancelled')) {
                if ($null -eq $cut.$field) {throw "Missing page-worker field: $field"}
            }
            if ($Clod) {
                foreach ($field in @('clodRamAdapterVersion','clodRamKnownSourceBytes','pageSourceKnownBytes','selectedCutSha256','sourceSha256','fallbackSelected')) {
                    if ($null -eq $cut.$field) {throw "Missing selected-CLOD page field: $field"}
                }
                if ($cut.clodRamAdapterVersion -ne 1 -or $cut.clodRamKnownSourceBytes -le 0 -or $cut.clodRamKnownSourceBytes -gt 131072 -or
                    $cut.pageSourceKnownBytes -le 0 -or $cut.pageSourceKnownBytes -gt 131072 -or
                    $cut.selectedCutSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $cut.selectedCutSha256 -eq $cut.sourceSha256) {
                    throw 'Selected-cut page provenance/capacity is missing or aliases raw source identity'
                }
            }
            if (!$cut.pagedPilot -or $cut.pageEpoch -lt 1 -or $cut.pageLiveJobs -gt 2 -or
                $cut.pageQueued -gt 1 -or $cut.pageActive -gt 1 -or $cut.pageReady -gt 1 -or
                $cut.pageReservedBytes -gt 1048576) {throw "Invalid bounded page-worker cut: $($cut | ConvertTo-Json -Compress)"}
            return $cut
        }
        function WaitPage($predicate) {
            $until=[DateTime]::UtcNow.AddSeconds(15)
            do {
                $cut=PageCut (Action 'status')
                if (& $predicate $cut) {return $cut}
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            throw "Page-worker state did not settle: $($cut | ConvertTo-Json -Compress)"
        }
        $firstFallback=$null;$fallback=$null;$fallbackReturned=$null
        if ($Clod) {
            $firstFallback=PageCut (Observe $true $false)
            if (!$firstFallback.fallbackSelected) {throw 'Combined pilot did not begin on independent authored fallback'}
            Capture 'initial-fallback.png';$null=Action 'coarse'
        }
        $firstCoarse=PageCut (Observe $true $false)
        if ($Clod -and ($firstCoarse.fallbackSelected -or $firstCoarse.rendererModel -eq $firstFallback.rendererModel -or
            $firstCoarse.rendererInstance -ne $firstFallback.rendererInstance)) {throw 'Combined selected coarse did not preserve fixed fallback instance'}
        if ($firstCoarse.fineResident -or $firstCoarse.pageLiveJobs -or $firstCoarse.pageWorkState -ne 'Idle') {
            throw 'Paged begin did not publish an independent coarse-only fallback'
        }
        Capture 'initial-coarse.png'
        $null=Action 'holdFine';$requested=PageCut (Action 'requestFine')
        if ($requested.pageRequest -lt 1) {throw 'No explicit page request was accepted'}
        $held=WaitPage {param($c) $c.pageActive -eq 1}
        if ($held.fineResident -or $held.fineSelected -or $held.pageReady -or
            $held.producerModel -ne $firstCoarse.producerModel -or $held.pageReservedBytes -le 0) {
            throw 'A held CPU decode replaced the coarse fallback or discarded active-job debt'
        }
        # Cancel the old fixture while its real worker is held, retire actual GPU
        # records, then queue a replacement epoch. The worker never owns Shape/FFI.
        $null=Action 'abort';$oldRetired=PageCut (Observe $false $false)
        if ($oldRetired.pageActive -ne 1 -or $oldRetired.pageLiveJobs -ne 1 -or $oldRetired.pageReservedBytes -le 0) {
            throw 'Abort released held worker ownership before the worker actually exited'
        }
        $replacement=PageCut (Action $beginAction)
        if ($Clod) {
            $fallback=PageCut (Observe $true $false)
            if (!$fallback.fallbackSelected) {throw 'Replacement epoch lost independent authored fallback'}
            $null=Action 'coarse'
        }
        $coarse=PageCut (Observe $true $false)
        if ($Clod -and ($coarse.fallbackSelected -or $coarse.rendererModel -eq $fallback.rendererModel -or
            $coarse.rendererInstance -ne $fallback.rendererInstance -or $coarse.producerInstance -ne $fallback.producerInstance)) {
            throw 'Replacement selected coarse aliases fallback or changes instance'
        }
        if ($coarse.pageEpoch -eq $firstCoarse.pageEpoch -or $coarse.fineResident -or
            $coarse.producerModel -eq $firstCoarse.producerModel) {throw 'Replacement did not create a fresh coarse epoch'}
        $queued=PageCut (Action 'requestFine')
        if ($queued.pageRequest -lt 1 -or $queued.pageRequest -eq $requested.pageRequest -or
            $queued.pageActive -ne 1 -or $queued.pageQueued -ne 1 -or $queued.pageLiveJobs -ne 2) {
            throw 'Replacement was not bounded to one cancelled active plus one new queued job'
        }
        $null=Action 'releaseFine'
        $until=[DateTime]::UtcNow.AddSeconds(15)
        do {
            $polled=PageCut (Action 'pollFine');$state=PageCut (Action 'status')
            if ($state.pageWorkState -eq 'Failed') {throw "Fine transaction refused: $($state | ConvertTo-Json -Compress)"}
            if ($state.fineResident) {break}
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $until)
        if (!$state.fineResident -or $state.pageWorkState -ne 'Resident') {throw 'Complete fresh fine result was not admitted before deadline'}
        $fine=PageCut (Observe $true $true)
        if ($fine.pageEpoch -ne $coarse.pageEpoch -or $fine.pageRequest -ne $queued.pageRequest -or
            $fine.pageCancelled -le $firstCoarse.pageCancelled -or $fine.pageCompleted -lt 1 -or
            $fine.meshCount -le $coarse.meshCount -or $fine.rendererModel -eq $coarse.rendererModel -or
            $fine.producerInstance -ne $coarse.producerInstance -or $fine.rendererInstance -ne $coarse.rendererInstance) {
            throw 'Fine was not a fresh complete worker result on the fixed replacement instance'
        }
        if ($Clod -and ($fine.fallbackSelected -or $fine.rendererModel -eq $fallback.rendererModel -or
            $fine.selectedCutSha256 -ne $coarse.selectedCutSha256 -or $fine.sourceSha256 -ne $coarse.sourceSha256)) {
            throw 'Worker fine aliases fallback or changed immutable selected/raw source identity'
        }
        Capture 'fine.png'
        $null=Action 'coarse';$returned=PageCut (Observe $true $false)
        if (!$returned.fineResident -or $returned.rendererModel -ne $coarse.rendererModel -or
            $returned.rendererInstance -ne $coarse.rendererInstance) {throw 'Independently resident coarse fallback was lost'}
        if ($Clod -and $returned.fallbackSelected) {throw 'Returned selected coarse incorrectly marked authored fallback'}
        Capture 'coarse.png'
        $null=Action 'fine';$fineReturned=PageCut (Observe $true $true)
        if ($fineReturned.rendererModel -ne $fine.rendererModel -or $fineReturned.rendererInstance -ne $fine.rendererInstance) {
            throw 'Explicit fine switch changed the committed model or instance'
        }
        $eviction=$null
        if ($Evict) {
            $null=Action 'coarse';$beforeEvict=PageCut (Observe $true $false)
            if ($beforeEvict.fineGeneration -ne 1 -or !$beforeEvict.fineResident -or $beforeEvict.fineEvictionCount -ne 0) {
                throw 'Initial fine generation did not qualify for one-cycle eviction'
            }
            if ($Clod -and $beforeEvict.fallbackSelected) {throw 'Eviction attempted from authored fallback rather than selected coarse'}
            $started=PageCut (Action 'evictFine')
            if ($started.status -ne 'Pending' -or !$started.fineEvictionPending -or $started.fineResident -or
                $started.pageRequest -ne 0 -or $started.pageWorkState -ne 'Evicting') {throw 'Fine retirement did not start transactionally'}
            $evicted=PageCut (Observe $true $false)
            if ($evicted.pageWorkState -ne 'Evicted' -or $evicted.fineEvictionPending -or
                $evicted.fineEvictionCount -ne 1 -or $evicted.retiredFineCount -lt 1 -or $evicted.fineResident -or
                $evicted.expectedPresentMeshes -ne $coarse.meshCount -or $evicted.expectedAbsentMeshes -ne $evicted.retiredFineCount -or
                $evicted.meshCount -ne $beforeEvict.meshCount -or $evicted.rendererModel -ne $coarse.rendererModel -or
                $evicted.rendererInstance -ne $coarse.rendererInstance -or $evicted.producerInstance -ne $coarse.producerInstance) {
                throw 'Exact old fine Absent cut did not preserve the coarse instance/history'
            }
            for ($i=0;$i -lt $beforeEvict.meshCount;++$i) {
                if ($evicted.rendererMeshHandles[$i] -ne $beforeEvict.rendererMeshHandles[$i]) {throw 'Eviction rewrote old full-generation history'}
            }
            Capture 'evicted-coarse.png'
            # Hold another fresh CPU request and cancel it without losing the
            # coarse cut. The reserved refill ID is not registered before full publication.
            $null=Action 'holdFine';$refillHeldRequest=PageCut (Action 'requestFine')
            $refillHeld=WaitPage {param($c) $c.pageActive -eq 1}
            $cancelled=PageCut (Action 'cancelFine')
            if ($cancelled.pageRequest -ne 0 -or $cancelled.fineResident -or $cancelled.pageReservedBytes -le 0) {
                throw 'Canceled refill lost fallback or dropped held-job debt early'
            }
            $cancelledCoarse=PageCut (Observe $true $false)
            $refill=PageCut (Action 'requestFine')
            if ($refill.pageRequest -le $refillHeldRequest.pageRequest -or $refill.pageRequest -le $queued.pageRequest -or
                $refill.pageQueued -ne 1 -or $refill.pageActive -ne 1 -or $refill.pageLiveJobs -ne 2) {throw 'Fresh refill did not use a distinct bounded request'}
            $null=Action 'releaseFine';$until=[DateTime]::UtcNow.AddSeconds(15)
            do {
                $null=Action 'pollFine';$state=PageCut (Action 'status')
                if ($state.pageWorkState -eq 'Failed') {throw 'Refill transaction refused'}
                if ($state.fineResident) {break}
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            if (!$state.fineResident) {throw 'Refill did not become resident'}
            $refilled=PageCut (Observe $true $true)
            if ($refilled.fineGeneration -ne 2 -or $refilled.fineEvictionCount -ne 1 -or
                $refilled.pageRequest -ne $refill.pageRequest -or $refilled.meshCount -le $evicted.meshCount -or
                $refilled.meshesAbsent -ne $evicted.retiredFineCount -or $refilled.rendererModel -eq $fine.rendererModel -or
                $refilled.producerModel -eq $fine.producerModel -or $refilled.rendererInstance -ne $coarse.rendererInstance -or
                $refilled.producerInstance -ne $coarse.producerInstance) {throw 'Refill did not publish a new complete private generation'}
            for ($i=0;$i -lt $evicted.meshCount;++$i) {
                if ($refilled.rendererMeshHandles[$i] -ne $evicted.rendererMeshHandles[$i]) {throw 'Refill erased/replaced old mesh-generation history'}
            }
            if ($Clod -and ($refilled.fallbackSelected -or $refilled.rendererModel -eq $fallback.rendererModel -or
                $refilled.selectedCutSha256 -ne $coarse.selectedCutSha256 -or $refilled.sourceSha256 -ne $coarse.sourceSha256)) {
                throw 'Refill aliases fallback or changed retained selected-cut/raw source provenance'
            }
            if ($Clod -and (@($fallback.rendererModel,$coarse.rendererModel,$fine.rendererModel,$refilled.rendererModel | Select-Object -Unique).Count -ne 4 -or
                @($fallback.producerModel,$coarse.producerModel,$fine.producerModel,$refilled.producerModel | Select-Object -Unique).Count -ne 4)) {
                throw 'Combined fallback/coarse/fine/refill model identities are not four distinct birth identities'
            }
            Capture 'refilled-fine.png'
            $null=Action 'coarse';$coarseAfterRefill=PageCut (Observe $true $false)
            if ($coarseAfterRefill.rendererModel -ne $coarse.rendererModel -or $coarseAfterRefill.rendererInstance -ne $coarse.rendererInstance) {
                throw 'Refill changed the independent coarse model or fixed instance'
            }
            if ($Clod -and $coarseAfterRefill.fallbackSelected) {throw 'Refill return incorrectly selected authored fallback'}
            $secondEvict=Action 'evictFine'
            if ($secondEvict.status -ne 'Invalid') {throw 'A second eviction exceeded the one-cycle scope'}
            $eviction=@{before=$beforeEvict;started=$started;evicted=$evicted;heldRequest=$refillHeldRequest;held=$refillHeld;
                cancelled=$cancelled;cancelledCoarse=$cancelledCoarse;request=$refill;refilled=$refilled;coarse=$coarseAfterRefill;secondEvict=$secondEvict}
        }
        if ($Clod) {
            $null=Action 'fallback';$fallbackReturned=PageCut (Observe $true $false)
            if (!$fallbackReturned.fallbackSelected -or $fallbackReturned.rendererModel -ne $fallback.rendererModel -or
                $fallbackReturned.producerModel -ne $fallback.producerModel -or
                $fallbackReturned.rendererInstance -ne $fallback.rendererInstance -or
                $fallbackReturned.producerInstance -ne $fallback.producerInstance) {throw 'One-cycle paging lost independent authored fallback'}
            Capture 'returned-fallback.png'
        }
        $null=Action 'abort';$retired=PageCut (Observe $false $false)
        $settled=WaitPage {param($c) $c.pageLiveJobs -eq 0 -and $c.pageQueued -eq 0 -and
            $c.pageActive -eq 0 -and $c.pageReady -eq 0 -and $c.pageReservedBytes -eq 0}
        $result=@{begin=$begin;firstCoarse=$firstCoarse;requested=$requested;held=$held;oldRetired=$oldRetired;
            replacement=$replacement;coarse=$coarse;queued=$queued;fine=$fine;returned=$returned;fineReturned=$fineReturned;
            retired=$retired;settled=$settled;eviction=$eviction;disabled=$false;paged=$true;clod=[bool]$Clod;
            firstFallback=$firstFallback;fallback=$fallback;fallbackReturned=$fallbackReturned}
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
    $result.passed=$true;$result.cleanExit=$true;$result.scope='Private original authored resident geometry, record lifetime and fixed-instance switching; explicit authored or selected-CLOD RAM worker paging only when selected; no generalized asset/DAG paging, all-pass, device-free or performance acceptance'
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
