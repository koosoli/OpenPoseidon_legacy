<#
.SYNOPSIS
Capture serial native grass filter controls with frozen weather, time and wind.
.DESCRIPTION
Use scripts/with-game-lock.sh against an installed matched pair. Reuses the
terrain stability runner's TCP lifecycle and image metrics, with fresh profiles
and repeated close/aerial images. This is appearance evidence, not moving-wind,
adjacent-frame shimmer, or performance acceptance. The current native harness
has no capture_metrics command; startup placement logs cannot prove per-pose
population equality. That limitation is explicitly retained in result.json.
#>
[CmdletBinding()]
param(
    [string]$CloseCamera = '9514.25 2994.25 211.72 90 -18',
    [string]$AerialCamera = '9514.25 2994.25 244.72 90 -86',
    [ValidateRange(15,90)][int]$WarmupSeconds = 35,
    [ValidateRange(1,15)][int]$SettleSeconds = 3,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'native-static',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
function Camera-Numbers([string]$value) {
    $tokens = @($value.Trim() -split '\s+')
    if ($tokens.Count -ne 5) { throw 'Camera requires X Z absolute-height azimuth elevation.' }
    foreach ($token in $tokens) {
        $number = 0.0
        if (![double]::TryParse($token,[Globalization.NumberStyles]::Float,$culture,[ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "Invalid camera number: $token" }
    }
    return $tokens
}
$close = @(Camera-Numbers $CloseCamera); $aerial = @(Camera-Numbers $AerialCamera)
$root = Split-Path -Parent $PSScriptRoot
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in @($mission,$NativeAddons)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required fixture path missing: $path" }
}
function Installed-State {
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Empty installed deployment provenance.' }
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name=$_; path=$file.FullName; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes=$file.Length; writtenUtc=$file.LastWriteTimeUtc.ToString('o') }
    }
    return @{deployedFrom=$stamp; files=@($files)}
}
$before = Installed-State
$output = Join-Path $root ('build/native-grass-static/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$settings = @{
    POSEIDON_REFORGER_WORLD='worlds/eden'; POSEIDON_REFORGER_OBJECTS='1'; POSEIDON_REFORGER_STREAM='1';
    WGR_NATIVE_TEXTURE_KEYS='1'; WGR_LOD_GOVERNOR_RANGE='1'; WGR_GRASS='1';
    POSEIDON_WIND_OVERRIDE='0 90 0'; WGR_CLOUD_COVERAGE='0'; WGR_TERRAIN_JITTER='0';
    WGR_TEMPORAL='0'; WGR_AUTO_EXPOSURE='0'; WGR_EXPOSURE='1';
    POSEIDON_TEST_RAIN='0'; WGR_TERRAIN_PUDDLES='0'; WGR_TERRAIN_PUDDLE_WETNESS='0';
    WGR_INTERIOR_SKY='1'; WGR_INTERIOR_SKY_DEBUG='0'; POSEIDON_VSYNC='0'
}
$keys = @($settings.Keys) + @('POSEIDON_USER_DIR','WGR_GRASS_FILTER_SUBPIXEL')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key]=[Environment]::GetEnvironmentVariable($key,'Process') }
$scope = 'Frozen native filter appearance controls; central 60% image metrics include ground, shadows and objects. No wind/shimmer/performance acceptance.'
$result = [ordered]@{status='running'; scope=$scope; before=$before; arms=@{}; comparisons=@{};
    populationProof='Unavailable per pose: source harness has no capture_metrics command; startup log witness only.'; error=$null}
@{scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash; sourceHead=[string](& git -C $root rev-parse HEAD);
    installed=$before; mission=$mission; nativeAddons=$NativeAddons; closeCamera=$CloseCamera; aerialCamera=$AerialCamera;
    lockOwner=$env:LOCK_OWNER; settings=$settings; inheritedEnvironment=$savedEnv; warmupSeconds=$WarmupSeconds;
    settleSeconds=$SettleSeconds; fixedSimTime=100; fixedDate=@(1985,6,21,16,0); brightness=1;
    width=1280; height=720; msaa=4; scope=$scope; populationProof=$result.populationProof
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json') -Encoding UTF8
if (!('TerrainPuddleMetrics' -as [type])) {
    Add-Type -AssemblyName System.Drawing
    $drawingRefs = @([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    $drawingRefs += @([Drawing.Bitmap].Assembly.GetReferencedAssemblies() |
        Where-Object Name -like 'System.Private.Windows.*' | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $drawingRefs
}
$p=$null; $client=$null; $writer=$null; $reader=$null; $log=''
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Runtime failure invalidates captures.'
    }
}
function Send($command) {
    Assert-RunHealth
    $request=$command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
    $writer.WriteLine($request)
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Harness response deadline exceeded.' }
        $line=$reader.ReadLine()
        if ($null -eq $line) { throw 'Harness connection closed.' }
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
        $reply=$line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok) { throw $line }
    return $reply
}
function Capture([string]$name,[string[]]$tokens) {
    $reply=Send @{cmd='eval';code=('triFreeFlyPose "'+($tokens -join ' ')+'"')}
    if ($reply.result -notmatch 'OK') { throw "Camera refused: $name" }
    Start-Sleep -Seconds $SettleSeconds
    $path=Join-Path $armDir ($name+'.png')
    $null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {
        Assert-RunHealth
        if ([DateTime]::UtcNow -gt $until) { throw "Missing capture: $path" }
        Start-Sleep -Milliseconds 100
    }
    $bytes=[IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG: $path" }
    $width=([int]$bytes[16]*16777216)+([int]$bytes[17]*65536)+([int]$bytes[18]*256)+[int]$bytes[19]
    $height=([int]$bytes[20]*16777216)+([int]$bytes[21]*65536)+([int]$bytes[22]*256)+[int]$bytes[23]
    if ($width -ne 1280 -or $height -ne 720) { throw 'Capture resolution differs from native 1280x720.' }
    $clock=Send @{cmd='eval';code='time'}
    $simTime=0.0
    if (![double]::TryParse(([string]$clock.result).Trim('"'),[Globalization.NumberStyles]::Float,$culture,[ref]$simTime) -or
        [double]::IsNaN($simTime) -or [double]::IsInfinity($simTime) -or [Math]::Abs($simTime-100) -gt .001) {
        throw 'Paused simulation time changed.'
    }
    return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;width=$width;height=$height;
        pose=($tokens -join ' ');simulationTime=$simTime;capturedUtc=[DateTime]::UtcNow.ToString('o');
        brightness=[TerrainPuddleMetrics]::Brightness($path);population=$result.populationProof}
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        $null=Send @{cmd='exec';code='setAccTime 1'}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
    }
    if (!$p -or $p.ExitCode -ne 0 -or !(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) {
        throw 'Clean normal shutdown proof missing.'
    }
    if ($client) { $client.Dispose(); $script:client=$null }
    $script:writer=$null; $script:reader=$null
}
try {
    foreach ($key in $settings.Keys) { [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process') }
    foreach ($filter in @('0','1')) {
        if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Unexpected game process between arms.' }
        $armDir=Join-Path $output ('filter'+$filter); $profile=Join-Path $armDir 'user'
        New-Item -ItemType Directory -Force -Path $profile | Out-Null
        & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
        [Environment]::SetEnvironmentVariable('POSEIDON_USER_DIR',$profile,'Process')
        [Environment]::SetEnvironmentVariable('WGR_GRASS_FILTER_SUBPIXEL',$filter,'Process')
        $log=Join-Path $armDir 'engine.log'; $stderr=Join-Path $armDir 'stderr.txt'; $stdout=Join-Path $armDir 'stdout.txt'
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
        $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
            '--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$NativeAddons+'"'),
            '--test-world-hour','16','--test-world-freefly')+$close+@('--log-file',('"'+$log+'"'))
        # GUI bootstrap recognizes redirected stdout, then reopens BOTH CRT
        # streams. Redirecting stderr alone can lose Rust's filter-mode witness.
        $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null=$p.Handle
        $armResult=@{pid=$p.Id;filter=$filter;arguments=$arguments;log=$log;stdout=$stdout;stderr=$stderr;captures=@{};normalShutdown=$false}
        $result.arms[$filter]=$armResult
        $until=[DateTime]::UtcNow.AddSeconds(120)
        do {
            Assert-RunHealth
            $client=[Net.Sockets.TcpClient]::new()
            try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client=$null }
            if (!$client) {
                if ([DateTime]::UtcNow -gt $until) { throw 'Harness unavailable after 120 seconds.' }
                Start-Sleep -Milliseconds 250
            }
        } while (!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000
        $reader=[IO.StreamReader]::new($stream)
        $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        Start-Sleep -Seconds $WarmupSeconds
        $ready=Send @{cmd='eval';code='triSceneReady'}
        if ($ready.result -notmatch 'OK') { throw 'Mission scene not ready.' }
        $description=Send @{cmd='describe'}
        $description | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $armDir 'harness-commands.json') -Encoding UTF8
        $armResult.captureMetricsCommandAdvertised=('capture_metrics' -in @($description.commands | ForEach-Object name))
        $null=Send @{cmd='exec';code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]'}
        foreach ($expression in @('triSetSimTime 100','triSetBrightness 1')) {
            $reply=Send @{cmd='eval';code=$expression}
            if ($reply.result -notmatch 'OK') { throw "Deterministic setup refused: $expression" }
        }
        $armResult.captures.close=Capture 'close-before' $close
        $armResult.captures.closeStatic=Capture 'close-static' $close
        $armResult.captures.aerial=Capture 'aerial-before' $aerial
        $armResult.captures.aerialStatic=Capture 'aerial-static' $aerial
        $armResult.captures.closeReturn=Capture 'close-return' $close
        $armResult.captures.aerialReturn=Capture 'aerial-return' $aerial
        $weather=Send @{cmd='weather_visibility'}
        if ($weather.rain -gt .001 -or $weather.fog -gt .001) { throw 'Actual rain/fog invalidates static controls.' }
        $armResult.weather=$weather
        Close-Owned
        $armResult.exitCode=$p.ExitCode;$armResult.normalShutdown=$true
        if (!(Select-String -LiteralPath $log -Pattern 'near blades from Reforger PlantMat' -Quiet)) { throw 'Native PlantMat source proof missing.' }
        $mode=if ($filter -eq '1') {'true'} else {'false'}
        $proof="Wgpu grass subpixel shading: $mode (WGR_GRASS_FILTER_SUBPIXEL)"
        if (!(Select-String -LiteralPath $stderr -Pattern $proof -SimpleMatch -Quiet)) { throw 'Requested filter mode proof missing.' }
        $witnesses=@(Select-String -LiteralPath $log -Pattern 'Wgpu grass placement: near (\d+)/(\d+) mid (\d+)/(\d+) far (\d+)/(\d+)')
        $armResult.startupPlacementWitness=@($witnesses | ForEach-Object { @{line=$_.Line;lineNumber=$_.LineNumber} })
        if (!@($witnesses | Where-Object { [long]$_.Matches[0].Groups[1].Value -gt 0 -and [long]$_.Matches[0].Groups[3].Value -gt 0 }).Count) {
            throw 'No log witness of populated native near and mid grass; cannot compare an empty field.'
        }
        $armResult.staticNoise=@{
            close=[TerrainPuddleMetrics]::Compare($armResult.captures.close.path,$armResult.captures.closeStatic.path,8);
            aerial=[TerrainPuddleMetrics]::Compare($armResult.captures.aerial.path,$armResult.captures.aerialStatic.path,8);
            closeReturn=[TerrainPuddleMetrics]::Compare($armResult.captures.close.path,$armResult.captures.closeReturn.path,8);
            aerialReturn=[TerrainPuddleMetrics]::Compare($armResult.captures.aerial.path,$armResult.captures.aerialReturn.path,8)
        }
        $armResult | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $armDir 'result.json') -Encoding UTF8
    }
    foreach ($view in @('close','closeStatic','closeReturn','aerial','aerialStatic','aerialReturn')) {
        $result.comparisons[$view]=[TerrainPuddleMetrics]::Compare($result.arms['0'].captures[$view].path,$result.arms['1'].captures[$view].path,8)
    }
    $result.status='captured';$result.appearance='awaiting judgement of controlled images; pixel difference is not a grass segmentation'
} catch {
    $result.status='failed';$result.error=$_.Exception.Message
    throw
} finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill();$null=$p.WaitForExit(5000);$result.forcedOwnedProcessStop=$true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after=Installed-State;$result.after=$after
        $changed=$after.deployedFrom -cne $before.deployedFrom
        for ($i=0;$i -lt $before.files.Count;++$i) {
            foreach ($field in @('name','path','sha256','bytes','writtenUtc')) {
                if ($after.files[$i][$field] -cne $before.files[$i][$field]) { $changed=$true }
            }
        }
        if ($changed) { $result.status='failed';$result.error='Installed stamp, EXE or DLL changed during capture.' }
    } catch { $result.status='failed';$result.error=$_.Exception.Message }
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) { Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    }
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
    Write-Host "Native static grass evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
}
