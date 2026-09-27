function New-GeometryDiskProducerProof([string]$Tools,[string]$Out) {
    $Tools=[IO.Path]::GetFullPath($Tools)
    if (!(Test-Path -LiteralPath $Tools -PathType Leaf)) {throw "Missing offline producer: $Tools"}
    $directory=Join-Path $Out 'offline-clod'
    $path=Join-Path $directory 'selected.gcd'
    if ($path.Length -gt 1023 -or $path -match '[^\x20-\x7e]') {throw 'Disk fixture path must be bounded ASCII'}
    $stdout=Join-Path $Out 'producer-stdout.json';$stderr=Join-Path $Out 'producer-stderr.txt'
    $initialHash=(Get-FileHash -LiteralPath $Tools -Algorithm SHA256).Hash
    $producer=$null
    try {
        $producer=Start-Process -FilePath $Tools -ArgumentList @('geometry-page-pilot','--output-directory',('"'+$directory+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $until=[DateTime]::UtcNow.AddSeconds(60)
        while (!$producer.HasExited) {
            foreach ($file in @($stdout,$stderr)) {if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).Length -gt 8192) {throw 'Producer diagnostic output exceeded8192bytes'}}
            if ([DateTime]::UtcNow -gt $until) {throw 'Offline producer exceeded60seconds'}
            Start-Sleep -Milliseconds 100
        }
        $producer.WaitForExit()
        if ($producer.ExitCode -ne 0) {throw "Offline producer failed: $($producer.ExitCode)"}
    } finally {if ($producer -and !$producer.HasExited) {Stop-Process -Id $producer.Id -Force}}
    foreach ($file in @($stdout,$stderr,(Join-Path $directory 'manifest.json'))) {
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -gt 8192) {throw 'Missing or oversized producer manifest/diagnostic'}
    }
    if ((Get-FileHash -LiteralPath $Tools -Algorithm SHA256).Hash -ne $initialHash) {throw 'Producer changed during offline operation'}
    $value=Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    function Keys($object,$expected) {
        if ($null -eq $object) {throw 'Null producer object'}
        $actual=@($object.PSObject.Properties.Name | Sort-Object);$wanted=@($expected | Sort-Object)
        if (($actual -join '|') -ne ($wanted -join '|')) {throw 'Incomplete or unexpected producer key set'}
        foreach ($key in $expected) {if ($null -eq $object.$key) {throw "Null producer field: $key"}}
    }
    function Integer($value,[uint64]$min,[uint64]$max) {
        if ($value -is [string] -or $value -is [bool] -or $null -eq $value) {throw 'Producer integer type mismatch'}
        $number=[double]$value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt $min -or $number -gt $max -or [Math]::Floor($number) -ne $number) {throw 'Invalid producer integer'}
    }
    function Identity($identity) {
        Keys $identity @('sourceSha256','geometryOptionsHex','materialOptionsHex','producerVersion','coarseRepresentation','fineRepresentation','vertexLayout','materialMapping')
        if ($identity.sourceSha256 -notmatch '^[0-9a-f]{64}$' -or $identity.geometryOptionsHex -cne '0000000000000000' -or $identity.materialOptionsHex -cne '0000000000000000') {throw 'Invalid producer source identity'}
        foreach ($key in @('producerVersion','materialMapping')) {Integer $identity.$key 1 1}
        Integer $identity.coarseRepresentation 0 0;Integer $identity.fineRepresentation 1 1;Integer $identity.vertexLayout 68 68
    }
    Keys $value @('schemaVersion','controlledPilotVersion','file','fileBytes','fileSha256','diskCodecSchema','sVertexBytes','sVertexLayoutKeyHex','clodLibraryRevision','clodAdapterVersion','ramAdapterVersion','originalSource','selectedCut','coarseThresholdBits','fineThresholdBits','coarsePages','finePages','coarseTriangles','fineTriangles','originalFineVertices','originalFineTriangles','authoredFallbackTriangles','selectedClusters','bakeGroups','bakeClusters','knownSourceBytes','helperSha256','scope')
    foreach ($key in @('schemaVersion','controlledPilotVersion','diskCodecSchema','clodAdapterVersion','ramAdapterVersion','coarsePages','finePages')) {Integer $value.$key 1 1}
    Integer $value.fileBytes 71652 71652;Integer $value.sVertexBytes 68 68
    if ($value.file -cne 'selected.gcd' -or $value.fileSha256 -notmatch '^[0-9a-f]{64}$' -or $value.helperSha256 -notmatch '^[0-9a-f]{64}$' -or
        $value.sVertexLayoutKeyHex -cne 'df19d63f75aab18d' -or $value.clodLibraryRevision -cne '9e1f07b159d3cb777f1c67ed31fc11fd117986f4' -or
        $value.scope -cne 'original-controlled-pilot-only; external-producer-manifest; no-retail-eligibility-or-GPU-claim') {throw 'Pinned producer format/layout/scope mismatch'}
    Identity $value.originalSource;Keys $value.selectedCut @('source','formatVersion','algorithmVersion','packing');Identity $value.selectedCut.source
    Integer $value.selectedCut.formatVersion 1 1;Integer $value.selectedCut.algorithmVersion 2 2
    Keys $value.selectedCut.packing @('clusterVertices','clusterTriangles','pageBytes')
    Integer $value.selectedCut.packing.clusterVertices 64 64;Integer $value.selectedCut.packing.clusterTriangles 124 124;Integer $value.selectedCut.packing.pageBytes 65536 65536
    if ($value.originalSource.sourceSha256 -eq $value.selectedCut.source.sourceSha256) {throw 'Raw and selected-cut identities alias'}
    Integer $value.coarseThresholdBits 1 2139095039;Integer $value.fineThresholdBits 0 0
    Integer $value.coarseTriangles 1 511;Integer $value.fineTriangles 512 512;Integer $value.originalFineTriangles 512 512;Integer $value.originalFineVertices 289 289;Integer $value.authoredFallbackTriangles 2 2
    Integer $value.selectedClusters 1 64;Integer $value.bakeGroups 2 64;Integer $value.bakeClusters 1 64;Integer $value.knownSourceBytes 1 131072
    function EqualJson($a,$b) {
        if ($null -eq $a -or $null -eq $b) {return $null -eq $a -and $null -eq $b}
        if ($a -is [pscustomobject] -and $b -is [pscustomobject]) {
            $keys=@($a.PSObject.Properties.Name);$left=($keys | Sort-Object) -join '|';$right=($b.PSObject.Properties.Name | Sort-Object) -join '|'
            if ($left -cne $right) {return $false}
            foreach ($key in $keys) {if (!(EqualJson $a.$key $b.$key)) {return $false}};return $true
        }
        return $a.GetType() -eq $b.GetType() -and $a -ceq $b
    }
    $manifest=Get-Content -LiteralPath (Join-Path $directory 'manifest.json') -Raw | ConvertFrom-Json
    if (!(EqualJson $value $manifest)) {throw 'File manifest differs semantically from separately captured producer stdout'}
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne 71652 -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $value.fileSha256) {throw 'Actual disk artifact size/hash mismatch'}
    return @{path=$path;producer=$value;toolsPath=$Tools;toolsSha256=$initialHash;stdoutBytes=(Get-Item -LiteralPath $stdout).Length;fileSha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}


function New-GeometryDiskFaultProof($Proof,[string]$Out,[ValidateSet('Missing','Corrupt')][string]$Mode) {
    # Called only AFTER trusted fresh producer stdout + original file/manifest verification.
    $original=[IO.Path]::GetFullPath($Proof.path);$outRoot=[IO.Path]::GetFullPath($Out).TrimEnd('\','/')
    $prefix=$outRoot+[IO.Path]::DirectorySeparatorChar
    if (!$original.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
        !(Test-Path -LiteralPath $original -PathType Leaf) -or (Get-Item -LiteralPath $original).Length -ne 71652 -or
        (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ine $Proof.producer.fileSha256) {
        throw 'Trusted original disk producer artifact changed before fault injection'
    }
    $directory=Join-Path $outRoot ('disk-fault-'+$Mode.ToLowerInvariant())
    if (Test-Path -LiteralPath $directory) {throw 'Fault directory must be a new private child'}
    $null=[IO.Directory]::CreateDirectory($directory)
    $path=[IO.Path]::GetFullPath((Join-Path $directory 'selected.gcd'))
    if (!$path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or $path.Length -gt 1023 -or $path -match '[^\x00-\x7f]') {
        throw 'Fault path is outside private output or unsupported'
    }
    $faultHash=$null;$offset=$null;$before=$null;$after=$null
    if ($Mode -eq 'Corrupt') {
        # Magic-byte mutation guarantees Invalid, not an unrelated Unsupported version refusal.
        $bytes=[IO.File]::ReadAllBytes($original)
        if ($bytes.Length -ne 71652) {throw 'Original changed during bounded copy'}
        $offset=0;$before=[int]$bytes[0];$bytes[0]=[byte]($before -bxor 1);$after=[int]$bytes[0]
        [IO.File]::WriteAllBytes($path,$bytes)
        if ((Get-Item -LiteralPath $path).Length -ne 71652) {throw 'Fault copy size changed'}
        $faultHash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($faultHash -ieq $Proof.producer.fileSha256) {throw 'Corruption did not change content'}
    } elseif (Test-Path -LiteralPath $path) {throw 'Missing fault path unexpectedly exists'}
    $originalHash=(Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash
    if ($originalHash -ine $Proof.producer.fileSha256) {throw 'Fault injection modified the trusted original'}
    return @{mode=$Mode;path=$path;originalPath=$original;originalSha256=$originalHash;faultSha256=$faultHash;
        expectedReadStatus=if($Mode -eq 'Missing'){'Missing'}else{'Invalid'};byteOffset=$offset;byteBefore=$before;byteAfter=$after;
        originalBytes=71652;faultBytes=if($Mode -eq 'Corrupt'){71652}else{0};
        scope='New owned private absent path or exactly one changed magic byte in bounded copy; original producer stdout/manifest/file preserved'}
}
