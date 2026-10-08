#Requires -Version 7.0
<#
.SYNOPSIS
Installed acceptance of the persisted optional ReShade preference.
.DESCRIPTION
Run under scripts/with-game-lock.sh after deployment. Does not deploy, download,
modify the prepared runtime, or touch the player's profile. Three bounded child
launches test default off, prepared cache on, and missing-cache fallback. Only a
process started by this script may be terminated on a failed clean-exit timeout.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit,
    [string]$GameDir = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [string]$RuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'),
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
function Require($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
Require ([bool]$env:LOCK_OWNER) 'Run under scripts/with-game-lock.sh; shared game lock required.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved; close it before acceptance.'
$repo = Split-Path -Parent $PSScriptRoot
$stampPath = Join-Path $GameDir 'DEPLOYED-FROM.txt'
$stamp = (Get-Content -LiteralPath $stampPath -Raw).Trim()
$stampCommit = ($stamp -split '\s+')[1]
Require ($stampCommit -and ($stampCommit.StartsWith($ExpectedCommit) -or $ExpectedCommit.StartsWith($stampCommit))) 'Unexpected installed source commit.'
$exe = Join-Path $GameDir 'OpenPoseidon.exe'
Require (Test-Path -LiteralPath $exe -PathType Leaf) 'Installed OpenPoseidon.exe missing.'

# Engine preference discovery uses LOCALAPPDATA/OpenPoseidon/reshade/6.8.0.
# A custom prepared cache can be tested only if it retains that exact layout.
$runtime = [IO.Path]::GetFullPath($RuntimeDirectory)
$preparedLocal = [IO.Path]::GetFullPath((Join-Path $runtime '../../..'))
Require ($runtime.TrimEnd('\','/') -ieq (Join-Path $preparedLocal 'OpenPoseidon/reshade/6.8.0').TrimEnd('\','/')) 'RuntimeDirectory must end in OpenPoseidon/reshade/6.8.0.'
foreach ($file in @('ReShade64.dll','ReShade64.json','ReShade.ini','OpenPoseidon.ini','OpenPoseidonReShade.addon64','Shaders/OpenPoseidonLook.fx')) {
    Require (Test-Path -LiteralPath (Join-Path $runtime $file) -PathType Leaf) "Prepared optional cache missing $file."
}
Require ((Get-FileHash -LiteralPath (Join-Path $runtime 'ReShade64.dll')).Hash -ieq '0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7') 'Official ReShade DLL hash mismatch.'
Require ((Get-FileHash -LiteralPath (Join-Path $runtime 'ReShade64.json')).Hash -ieq 'aa21713718843e531da396e2bfc80772c9cb3c369d6c30b836be6b0ae812d503') 'Official ReShade manifest hash mismatch.'
$tag = (Get-Date -Format yyyyMMdd-HHmmss) + "-$PID"
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo "build/reshade-preference/$tag" }
$out = [IO.Path]::GetFullPath($OutputDirectory)
Require (!(Test-Path -LiteralPath $out)) 'OutputDirectory already exists; preserve previous evidence.'
$null = New-Item -ItemType Directory -Path $out
# Keep legacy profile/mission paths short even in a deeply nested worktree.
$lab = Join-Path ([IO.Path]::GetTempPath()) "op-rsp-$tag"
$null = New-Item -ItemType Directory -Path $lab
$mission = Join-Path $lab 'preference.eden'
$null = New-Item -ItemType Directory -Path $mission
@'
version=11;
class Mission {
 randomSeed=42;
 class Intel {year=1985;month=6;day=21;hour=16;minute=0;startWeather=0.25;forecastWeather=0.25;startFog=0.25;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
  position[]={8131.43,0,4086.37};azimut=229.1;id=0;side="WEST";vehicle="SoldierWB";
  player="PLAYER COMMANDER";leader=1;skill=1.0;
 };};};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ | Set-Content -LiteralPath (Join-Path $mission 'mission.sqm')
$receipt = @{installed=$stamp;passed=$false;profileRoot=$lab;arms=@();scope='Persisted optional runtime startup, fallback, live effects and actual display transition; not visual-quality/performance acceptance.'}
$cases = @(
    @{name='off';preference=$false;loaded=$false;local=$preparedLocal},
    @{name='prepared';preference=$true;loaded=$true;local=$preparedLocal},
    @{name='missing';preference=$true;loaded=$false;local=(Join-Path $lab 'empty-local')}
)
$critical = 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error:|Cannot load --test-world'
try {
    foreach ($case in $cases) {
        Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Another game appeared; preserving it.'
        $arm = Join-Path $out $case.name
        $user = Join-Path $lab ($case.name + '-user')
        $null = New-Item -ItemType Directory -Force -Path $arm,$user,$case.local
        & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
        Add-Content -LiteralPath (Join-Path $user 'graphics.cfg') -Value ("optionalReShade=" + [int]$case.preference + ';')
        $log = Join-Path $arm 'game.log'
        $record = @{name=$case.name;preference=$case.preference;expectedLoaded=$case.loaded;profile=$user;log=$log;passed=$false}
        $receipt.arms += $record
        $start = [Diagnostics.ProcessStartInfo]::new($exe)
        $start.WorkingDirectory = $GameDir
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        foreach ($key in @('VK_ADD_LAYER_PATH','VK_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE','RESHADE_BASE_PATH_OVERRIDE','WGPU_BACKEND','DISABLE_VK_LAYER_reshade_1')) {
            $null = $start.Environment.Remove($key)
        }
        $start.Environment['LOCALAPPDATA'] = $case.local
        $start.Environment['POSEIDON_USER_DIR'] = $user
        $start.Environment['POSEIDON_CACHE_DIR'] = Join-Path $user 'cache'
        $start.Environment['POSEIDON_TEMP_DIR'] = Join-Path $user 'temp'
        $start.Environment['POSEIDON_WHOLE_WORLD'] = '0'
        $start.Environment['WGR_DLSS'] = '0'
        $reserve = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $reserve.Start();$port = $reserve.LocalEndpoint.Port;$reserve.Stop()
        # No --test-world: stock OPRW2 Everon must use the ordinary mission loader.
        foreach ($argument in @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
                '--test-mission',$mission,'--harness',"$port",'--timeout','240','--log-file',$log)) {
            $start.ArgumentList.Add($argument)
        }
        $p = $null;$client = $null;$writer = $null;$reader = $null;$stdout = $null;$stderr = $null
        try {
            $p = [Diagnostics.Process]::Start($start)
            $record.pid = $p.Id
            $stdout = $p.StandardOutput.ReadToEndAsync();$stderr = $p.StandardError.ReadToEndAsync()
            $deadline = [DateTime]::UtcNow.AddSeconds(150)
            do {
                Require (!$p.HasExited) 'Game exited before harness readiness.'
                try { $client = [Net.Sockets.TcpClient]::new('127.0.0.1',$port) }
                catch { $client = $null;Start-Sleep -Milliseconds 250 }
                Require ([DateTime]::UtcNow -lt $deadline) 'Harness readiness deadline exceeded.'
            } while (!$client)
            $stream = $client.GetStream();$stream.ReadTimeout = 15000;$stream.WriteTimeout = 5000
            $reader = [IO.StreamReader]::new($stream)
            $writer = [IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush = $true
            function Send($Command) {
                $writer.WriteLine(($Command | ConvertTo-Json -Compress -Depth 8))
                do {
                    $line = $reader.ReadLine();Require ([bool]$line) 'Harness disconnected.'
                    $reply = $line | ConvertFrom-Json
                } while ($null -eq $reply.ok)
                Require $reply.ok $line
                return $reply
            }
            function EvalText([string]$Code) { return ([string](Send @{cmd='eval';code=$Code}).result).Trim('"') }
            while ((EvalText 'triSceneReady') -cne 'OK') {
                Require ([DateTime]::UtcNow -lt $deadline) 'Scene readiness deadline exceeded.'
                Start-Sleep -Milliseconds 250
            }
            Require (Select-String -LiteralPath $log -Pattern 'worlds[\\/]eden.wrp' -Quiet) 'Stock Everon landscape load missing.'
            Require (!(Select-String -LiteralPath $log -Pattern $critical -Quiet)) 'Scene load produced a critical error.'
            $state = Send @{cmd='optional_reshade';action='state'}
            if ($case.loaded) {
                $deadline = [DateTime]::UtcNow.AddSeconds(60)
                while (!$state.ready -or $state.techniques -lt 1) {
                    Require ([DateTime]::UtcNow -lt $deadline) 'Prepared preference never produced a ready effect runtime.'
                    Start-Sleep -Milliseconds 250;$state = Send @{cmd='optional_reshade';action='state'}
                }
            } else {
                Require (!$state.ready) 'Disabled/missing-cache preference unexpectedly loaded the runtime.'
            }
            $record.initial = $state
            $p.Refresh()
            $modules = @($p.Modules | Where-Object {$_.ModuleName -match '^ReShade64\.dll$|^OpenPoseidonReShade\.addon64$'} | ForEach-Object FileName)
            $record.modules = $modules
            if ($case.loaded) {
                Require (@($modules | Where-Object {[IO.Path]::GetFullPath($_) -ieq (Join-Path $runtime 'ReShade64.dll')}).Count -eq 1) 'Prepared official runtime DLL not loaded from the expected cache.'
                Require (Select-String -LiteralPath $log -SimpleMatch 'Optional ReShade requested for this launch (Vulkan).' -Quiet) 'Persisted opt-in startup confirmation missing.'
                foreach ($enabled in @($false,$true)) {
                    $null = Send @{cmd='optional_reshade';action=$(if ($enabled) {'on'} else {'off'})}
                    $deadline = [DateTime]::UtcNow.AddSeconds(15)
                    do {
                        Start-Sleep -Milliseconds 100;$state = Send @{cmd='optional_reshade';action='state'}
                        Require ([DateTime]::UtcNow -lt $deadline) 'Live effect toggle deadline exceeded.'
                    } while ($state.enabled -ne $enabled)
                    $record[$(if ($enabled) {'on'} else {'off'})] = $state
                }
                Require ($state.activeTechniques -gt 0) 'Enabled runtime has no selected effect.'
                $record.windowBefore = EvalText 'triGetWindowSize'
                Require ((EvalText 'triIsWindowed') -ceq 'OK:windowed') 'Fixture did not start windowed.'
                foreach ($windowed in @($false,$true)) {
                    $null = Send @{cmd='eval';code='triSendAltEnter'}
                    $expected = $(if ($windowed) {'OK:windowed'} else {'OK:fullscreen'})
                    $deadline = [DateTime]::UtcNow.AddSeconds(30)
                    do {
                        Start-Sleep -Milliseconds 250
                        $mode = EvalText 'triIsWindowed';$state = Send @{cmd='optional_reshade';action='state'}
                        Require ([DateTime]::UtcNow -lt $deadline) 'Display transition/runtime recovery deadline exceeded.'
                    } while ($mode -cne $expected -or !$state.ready -or $state.techniques -lt 1)
                    $size = EvalText 'triGetWindowSize'
                    Require ($size -match '^[1-9][0-9]*x[1-9][0-9]*$') 'Invalid render dimensions after display transition.'
                    $record[$(if ($windowed) {'windowAfter'} else {'fullscreen'})] = @{mode=$mode;size=$size;state=$state}
                    if (!$windowed) { Require ($size -cne $record.windowBefore) 'Alt+Enter did not change render dimensions; resize was not exercised.' }
                }
                $before = $state.presentCount;Start-Sleep -Milliseconds 500
                $state = Send @{cmd='optional_reshade';action='state'}
                Require ($state.ready -and $state.presentCount -gt $before) 'Presentation did not resume after display transitions.'
                $record.final = $state
            } else {
                Require ($modules.Count -eq 0) 'Native fallback unexpectedly loaded ReShade DLL/addon.'
                if ($case.name -ceq 'missing') {
                    Require (Select-String -LiteralPath $log -SimpleMatch 'ReShade cache missing or mismatched.' -Quiet) 'Missing-cache warning absent.'
                }
            }
            $record.performance = Send @{cmd='frame_performance'}
            $null = Send @{cmd='exit'}
            Require ($p.WaitForExit(10000)) 'Clean exit deadline exceeded.'
            $record.exitCode = $p.ExitCode
            Require ($p.ExitCode -eq 0) "Game exit code $($p.ExitCode)."
            Require (!(Select-String -LiteralPath $log -Pattern $critical -Quiet)) 'Renderer/runtime error during acceptance.'
        } catch { $record.error = $_.Exception.Message;throw }
        finally {
            if ($p -and !$p.HasExited) {
                # This is strictly the Process object returned by our own Start.
                try { if ($writer) { $writer.WriteLine('{"cmd":"exit"}') } } catch {}
                if (!$p.WaitForExit(5000)) { $p.Kill();$null = $p.WaitForExit(10000) }
            }
            if ($p -and $p.HasExited) {
                if ($stdout) { [IO.File]::WriteAllText((Join-Path $arm 'stdout.log'),$stdout.GetAwaiter().GetResult()) }
                if ($stderr) { [IO.File]::WriteAllText((Join-Path $arm 'stderr.log'),$stderr.GetAwaiter().GetResult()) }
            }
            if ($client) { $client.Dispose() }
            Copy-Item -LiteralPath (Join-Path $user 'graphics.cfg') -Destination (Join-Path $arm 'graphics.cfg')
        }
        Require (!(Select-String -LiteralPath (Join-Path $arm 'stderr.log') -Pattern $critical -Quiet)) 'Renderer stderr error.'
        $preferencePattern = 'optionalReShade\s*=\s*' + [int]$case.preference + '\s*;'
        Require (Select-String -LiteralPath (Join-Path $arm 'graphics.cfg') -Pattern $preferencePattern -Quiet) 'Saved preference did not survive startup/exit.'
        if ($case.loaded) {
            Copy-Item -LiteralPath (Join-Path $runtime 'ReShade.log') -Destination (Join-Path $arm 'ReShade.log')
            Require (!(Select-String -LiteralPath (Join-Path $arm 'ReShade.log') -Pattern 'Failed to compile|error [XC][0-9]|Failed to create effect' -Quiet)) 'ReShade effect compilation/runtime error.'
        }
        Require ((Get-Content -LiteralPath $stampPath -Raw).Trim() -ceq $stamp) 'Installed source changed during acceptance.'
        $record.passed = $true
    }
    $receipt.passed = $true
    Write-Host "ReShade preference acceptance PASS: $out"
} catch { $receipt.error = $_.Exception.Message;throw }
finally { $receipt | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $out 'result.json') }
