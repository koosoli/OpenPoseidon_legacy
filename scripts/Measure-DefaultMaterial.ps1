param([ValidateSet('Original','Albedo','Full')][string]$Arm = 'Full',
      [ValidateSet('Road','Sand','Gravel')][string]$Surface = 'Road',
      [ValidateRange(0,23.999)][double]$Hour = 14,
      [ValidateSet(0,1)][int]$DetailNormals = 1,
      [ValidateSet('Native','DLSS')][string]$Upscaler = 'Native',
      [switch]$Closeup,
      [ValidateSet('EdenPs','AbelPi','NoePt','NoePs')][string]$SandSource = 'EdenPs',
      [ValidateSet('Legacy','Near','Mid','Far')][string]$SandView = 'Legacy',
      [switch]$PlanOnly,
      [switch]$SelfTest,
      [string]$ToolsExe = '')
$ErrorActionPreference = 'Stop'
function Assert-SandSource($source,$site) {
    if ($null-eq $source -or $source.ok -ne $true -or $source.sourceEligible -ne $true -or
        (($source.world -replace '\\','/') -notmatch ('(^|/)'+$site.world+'\.wrp$')) -or $source.texture -ine $site.texture -or
        $source.surfaceClass -ine $site.surfaceClass -or $source.files -cne $site.files -or
        $source.sound -cne 'sand' -or $source.character -cne '' -or
        $source.x -ne $site.x -or $source.z -ne $site.z) {throw 'Actual target sand source/metadata mismatch'}
}
function Get-SandLoadedBindings([string]$log,$site) {
    $name=[regex]::Escape($site.texture);$albedo=[regex]::Escape($site.albedo)
    $options=[Text.RegularExpressions.RegexOptions]::IgnoreCase
    return @{
        originalAlbedoMapping=[regex]::IsMatch($log,'Default material: '+$name+' -> '+$albedo+' \(original texture identity preserved\)',$options)
        macro=[regex]::IsMatch($log,'Wgpu legacy terrain: '+$name+' enhanced by op_ground_materials\\sand_nohq\.paa',$options)
        detail=[regex]::IsMatch($log,'Wgpu legacy terrain detail: '+$name+' -> op_ground_materials\\sand_detail_nohq\.paa repeats/metre=2(?:\.0+)?(?:\s|$)',$options)
    }
}
function Assert-SandLoadedBindings($bindings,[string]$arm,[int]$detailNormals) {
    if ($arm -eq 'Original') {
        if ($bindings.originalAlbedoMapping -or $bindings.macro -or $bindings.detail) {throw 'Original arm unexpectedly binds enhanced sand'}
    } else {
        if (!$bindings.originalAlbedoMapping) {throw 'Original sand albedo mapping was not loaded'}
        if ($bindings.macro -ne ($arm -eq 'Full')) {throw 'Sand macro normal ON/OFF binding mismatch'}
        if ($bindings.detail -ne ($arm -eq 'Full' -and $detailNormals -eq 1)) {throw 'Sand detail normal ON/OFF binding mismatch'}
    }
}
if ($SelfTest) {
    $s=@{x=1;z=2;world='noe';texture='o\pt.paa';albedo='o\pt.paa';surfaceClass='SandDark';files='pt??????'}
    $actual=@{ok=$true;sourceEligible=$true;x=1;z=2;world='noe.wrp';texture='o\pt.paa';surfaceClass='SandDark';files='pt??????';sound='sand';character=''}
    Assert-SandSource $actual $s
    foreach ($negative in @(@{world='cain.wrp'},@{texture='o\ps.paa'},@{surfaceClass='Default'},@{files='default'},@{sourceEligible=$false},@{sound='rock'},@{character='grass'},@{x=3})) {
        $bad=$actual.Clone();foreach($key in $negative.Keys){$bad[$key]=$negative[$key]};$rejected=$false
        try {Assert-SandSource $bad $s} catch {$rejected=$true};if(!$rejected){throw 'Source falsifier did not reject'}
    }
    $mapping='Default material: o\pt.paa -> o\pt.paa (original texture identity preserved)'
    $macro='Wgpu legacy terrain: o\pt.paa enhanced by op_ground_materials\sand_nohq.paa'
    $detail='Wgpu legacy terrain detail: o\pt.paa -> op_ground_materials\sand_detail_nohq.paa repeats/metre=2'
    Assert-SandLoadedBindings (Get-SandLoadedBindings ($mapping+"`n"+$macro+"`n"+$detail) $s) 'Full' 1
    Assert-SandLoadedBindings (Get-SandLoadedBindings ($mapping+"`n"+$macro) $s) 'Full' 0
    Assert-SandLoadedBindings (Get-SandLoadedBindings $mapping $s) 'Albedo' 1
    Assert-SandLoadedBindings (Get-SandLoadedBindings '' $s) 'Original' 1
    foreach ($negative in @($mapping,($mapping+"`n"+$detail),($mapping+"`n"+$macro+"`n"+($detail -replace '=2$','=2.5')),($mapping -replace 'o\\pt','o\ps'))) {
        $rejected=$false;try {Assert-SandLoadedBindings (Get-SandLoadedBindings $negative $s) 'Full' 1} catch {$rejected=$true}
        if(!$rejected){throw 'Binding falsifier did not reject'}
    }
    $rejected=$false;try {Assert-SandLoadedBindings (Get-SandLoadedBindings $macro $s) 'Original' 1} catch {$rejected=$true}
    if(!$rejected){throw 'Original arm accepted a bound normal'}
    Write-Host 'Sand source/binding falsifiers PASS; no game, GPU, profile or archive mutations';return
}
if ($Closeup -and $Surface -ne 'Sand') { throw 'Closeup currently has a verified camera only for Sand' }
if (($SandSource -ne 'EdenPs' -or $SandView -ne 'Legacy') -and $Surface -ne 'Sand') { throw 'Sand source/view requires -Surface Sand' }
if ($SandSource -ne 'EdenPs' -and $SandView -eq 'Legacy') { throw 'Original-map sand additions require an explicit Near/Mid/Far view' }
if ($Closeup -and $SandView -ne 'Legacy') { throw 'Choose Closeup or SandView, not both' }
$root = Split-Path $PSScriptRoot -Parent
$sites = @{
    EdenPs=@{x=5975;z=3325;height=9.315;world='eden';texture='eden\ps.paa';albedo='op_ground_materials\sand_co.paa';surfaceClass='Sand';files='ps??????';archive='Dta/Eden.pbo';mission='perf_field.eden'}
    AbelPi=@{x=7575;z=10675;height=27.765;world='abel';texture='abel\pi.paa';surfaceClass='SandAbel';files='pi??????';archive='Dta/Abel.pbo';mission='perf_abel.abel'}
    NoePt=@{x=2575;z=5125;height=14.985;world='noe';texture='o\pt.paa';surfaceClass='SandDark';files='pt??????';archive='AddOns/O.pbo';mission='perf_sand.noe'}
    NoePs=@{x=2975;z=4125;height=39.015;world='noe';texture='o\ps.paa';surfaceClass='Sand';files='ps??????';archive='AddOns/O.pbo';mission='perf_sand.noe'}
}
$site = $sites[$SandSource]
if (!$site.ContainsKey('albedo')) {$site.albedo=$site.texture}
$mission = 'tests/perf/missions/perf_field.eden'
$pose = @('6378.7','5672.5','74.4','120','-12')
if ($Surface -eq 'Sand') {
    $pose = @('5800','3400','13.0','90','-20')
    if ($Closeup) { $pose = @('5800','3400','11.1','90','-60') }
    if ($SandView -ne 'Legacy') {
        $lift = @{Near=1.8;Mid=15.0;Far=50.0}[$SandView]
        $pose = @($site.x,($site.z-$lift/[Math]::Tan([Math]::PI/3)),($site.height+$lift),0,-60)
        $pose = @($pose | ForEach-Object { ([double]$_).ToString('0.######',[Globalization.CultureInfo]::InvariantCulture) })
        $mission = 'tests/perf/missions/' + $site.mission
    }
}
if ($Surface -eq 'Gravel') { $pose = @('2221.7','4788.8','40.2','230','-16') }
$plan = [ordered]@{arm=$Arm;surface=$Surface;sandSource=$SandSource;sandView=$SandView;site=$site;mission=$mission;freefly=$pose;hour=$Hour;detailNormals=$DetailNormals;upscaler=$Upscaler;scope='Source sample + CPU nonzero descriptor-table layer assignments + captures; no selected ground-draw or visual acceptance proof.'}
if ($PlanOnly) { $plan | ConvertTo-Json -Depth 8; return }
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
$hourText = $Hour.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
$label = 'material-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Surface + '-' + $Arm + '-h' + $hourText + '-d' + $DetailNormals
if ($Closeup) { $label += '-close' }
if ($Upscaler -eq 'DLSS') { $label += '-dlss' }
if ($SandView -ne 'Legacy') { $label += '-' + $SandSource + '-' + $SandView }
$out = Join-Path $root 'build/default-content/acceptance'
$user = Join-Path $out ('user-' + $label)
New-Item -ItemType Directory -Force $user | Out-Null
$dlssMode = if ($Upscaler -eq 'DLSS') {1} else {0}
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode $dlssMode
$settings = @{POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; WGR_LOD_GOVERNOR_RANGE='1';
    POSEIDON_VISUAL_UPGRADE_DETAIL_NORMALS="$DetailNormals";
    POSEIDON_VISUAL_UPGRADE_NORMALS=$(if ($Arm -eq 'Albedo') {'0'} else {'1'})}
$extra = @('--test-world-freefly') + $pose
$extra += @('--test-world-hour', $hourText)
if ($Arm -eq 'Original') { $extra += '--no-visual-upgrade' }
$probeJob = $null
$proofBefore = @{}
if ($SandView -ne 'Legacy') {
    $gameDir = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
    if ($Arm -ne 'Original') {
        if (!$ToolsExe) {$ToolsExe=Join-Path $root 'dist/x64-win-rwdi/PoseidonTools.exe'}
        $plan.installedPackagePreflight = & "$PSScriptRoot/Assert-DefaultMaterialPackage.ps1" `
            -Archive (Join-Path $gameDir 'Mods/@OP_VisualUpgrade/AddOns/op_ground_materials.pbo') `
            -ExpectedManifest (Join-Path $root 'content/default-packs/visual/material-overrides.json') `
            -ToolsExe $ToolsExe -EvidenceDirectory (Join-Path $out 'material-preflight')
    }
    foreach ($file in @('DEPLOYED-FROM.txt','OpenPoseidon.exe','wgpu_renderer.dll','Mods/@OP_VisualUpgrade/AddOns/op_ground_materials.pbo','BIN/CONFIG.BIN',$site.archive)) {
        $proofBefore[$file] = (Get-FileHash -LiteralPath (Join-Path $gameDir $file) -Algorithm SHA256).Hash
    }
    # Own only this read-only query job; farfield-bench retains game lifecycle ownership.
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $extra += @('--dev','--harness',"$port")
    $probeJob = Start-Job -ArgumentList $port,$site.x,$site.z,$site.world,$site.texture -ScriptBlock {
        param($port,$x,$z,$world,$texture)
        $ErrorActionPreference='Stop';$client=$null;$reader=$null;$writer=$null
        $deadline=[DateTime]::UtcNow.AddSeconds(100)
        try {
            do {
                $client=[Net.Sockets.TcpClient]::new()
                try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null}
                if (!$client) { Start-Sleep -Milliseconds 250 }
            } while (!$client -and [DateTime]::UtcNow -lt $deadline)
            if (!$client) {throw 'Sand source harness connection timed out'}
            $stream=$client.GetStream();$stream.ReadTimeout=10000
            $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
            do {
                $writer.WriteLine((@{cmd='dev_sand';action='sample';x=$x;z=$z}|ConvertTo-Json -Compress))
                do {$line=$reader.ReadLine();if ($null-eq $line){throw 'Sand source harness closed'};$reply=$line|ConvertFrom-Json} while ($null-eq $reply.ok)
                if ($reply.ok -and (($reply.world -replace '\\','/') -match ('(^|/)'+$world+'\.wrp$')) -and $reply.texture -ieq $texture) {
                    $sourceReply=$reply
                    $writer.WriteLine('{"cmd":"eval","code":"triSceneReady"}')
                    do {$line=$reader.ReadLine();if ($null-eq $line){throw 'Sand readiness harness closed'};$ready=$line|ConvertFrom-Json} while ($null-eq $ready.ok)
                    if ($ready.ok -and ([string]$ready.result).Trim('"') -ceq 'OK') {
                        $sourceReply | Add-Member -NotePropertyName queryUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
                        $sourceReply | Add-Member -NotePropertyName sceneReady -NotePropertyValue $ready.result
                        return ($sourceReply|ConvertTo-Json -Depth 8 -Compress)
                    }
                }
                Start-Sleep -Milliseconds 250
            } while ([DateTime]::UtcNow -lt $deadline)
            throw 'Sand source world query timed out'
        } finally {
            if ($writer) {$writer.Dispose()};if ($reader) {$reader.Dispose()};if ($client) {$client.Dispose()}
        }
    }
}
$benchmarkComplete=$false
try {
& "$PSScriptRoot/farfield-bench.ps1" -Label $label -Mission $mission `
    -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed -WarmupSeconds 20 `
    -TimeoutSeconds 120 -RequireAll -SampleMemory -ExtraArgs $extra
if ($LASTEXITCODE -ne 0) { throw "Material capture failed with exit code $LASTEXITCODE" }
$benchmarkComplete=$true
} finally {
    if ($probeJob -and $probeJob.State -eq 'Running') {Stop-Job -Job $probeJob}
    if ($probeJob -and !$benchmarkComplete) {Remove-Job -Job $probeJob -Force}
}
$sourceProbeJson=$null
if ($probeJob) {
    try {$sourceProbeJson=Receive-Job -Job $probeJob -ErrorAction Stop}
    finally {Remove-Job -Job $probeJob -Force}
}
$captureDir = Join-Path $out $label
& "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath (Join-Path $captureDir 'run-01.meta.json') -LogPath (Join-Path $captureDir 'run-01.log')
$capture = Get-Content -LiteralPath (Join-Path $captureDir 'run-01.json') -Raw | ConvertFrom-Json
if ($Upscaler -eq 'DLSS' -and $capture.build.dlss_active -ne $true) {
    throw "Requested DLSS was not active: $($capture.build.dlss_reason)"
}
if ($Upscaler -eq 'Native' -and ($capture.build.upscaler -ne 'native' -or
    $capture.build.render_width -ne $capture.build.output_width -or
    $capture.build.render_height -ne $capture.build.output_height)) {
    throw 'Requested native-resolution material capture used an upscaler or render scale'
}
if ($SandView -ne 'Legacy') {
        $source = $sourceProbeJson | ConvertFrom-Json
        $log = Get-Content -LiteralPath (Join-Path $captureDir 'run-01.log') -Raw
        $bindings=Get-SandLoadedBindings $log $site
        $plan.actualSource=$source;$plan.cpuBindings=$bindings;$plan.status='observed-not-validated'
        $plan | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $captureDir 'sand-source-bindings.json') -Encoding UTF8
        Assert-SandSource $source $site
        Assert-SandLoadedBindings $bindings $Arm $DetailNormals
        if ($log -match 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION') {throw 'Actual sand capture contains a renderer/runtime error'}
        $cameraRows=[regex]::Matches($log,'--test-world-freefly: camera at ([-+\d.eE]+), ([-+\d.eE]+), ([-+\d.eE]+); azimuth ([-+\d.eE]+) elevation ([-+\d.eE]+) degrees')
        if ($cameraRows.Count -ne 1) {throw 'Exactly one actual freefly camera receipt required'}
        $actualPose=@();for($i=1;$i-le 5;$i++){$actualPose += [double]::Parse($cameraRows[0].Groups[$i].Value,[Globalization.CultureInfo]::InvariantCulture)}
        $expectedPose=@($pose[0],$pose[2],$pose[1],$pose[3],$pose[4])
        for($i=0;$i-lt 5;$i++){if([Math]::Abs($actualPose[$i]-[double]::Parse($expectedPose[$i],[Globalization.CultureInfo]::InvariantCulture))-gt .02){throw 'Actual camera initialization differs from requested matched pose'}}
        foreach ($file in $proofBefore.Keys) {
            if ((Get-FileHash -LiteralPath (Join-Path $gameDir $file) -Algorithm SHA256).Hash -cne $proofBefore[$file]) {throw "Installed provenance changed: $file"}
        }
        $plan.status='source-and-loaded-bindings-pass-runtime-image-review-pending'
        $plan.actualCameraInitialization=$actualPose
        $plan.installedHashes=$proofBefore;$plan.scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash
        $plan | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $captureDir 'sand-source-bindings.json') -Encoding UTF8
}
