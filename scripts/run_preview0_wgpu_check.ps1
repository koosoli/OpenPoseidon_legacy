[CmdletBinding()]
param(
    [string]$BuildDir = "build\win-x64-clang-dbg",
    [string]$LogPath = "docs\roadmap\evidence\preview0-wgpu-check.log",
    [ValidateSet('wgpu', 'gl33')]
    [string]$Backend = 'wgpu',
    [switch]$SkipBuild,
    [switch]$LifecycleSmoke,
    [switch]$ProfileOverlaySmoke,
    [switch]$MissionCaptureSmoke
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot $BuildDir
$gameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$sourceExe = Join-Path $buildRoot 'apps\cwr\Game\OpenPoseidon.exe'
$sourceDll = Join-Path $buildRoot 'engine\WgpuRenderer\wgpu_renderer.dll'
# NEVER write ColdWarAssault.exe. That is the RETAIL binary's name; copying our build onto
# it is how a Poseidon build ends up wearing the retail name in a folder several worktrees
# share, and it is indistinguishable afterwards from the real thing.
$targetExe = Join-Path $gameDir 'OpenPoseidon.exe'
$targetDll = Join-Path $gameDir 'wgpu_renderer.dll'
$fullLogPath = if ([IO.Path]::IsPathRooted($LogPath)) { $LogPath } else { Join-Path $repoRoot $LogPath }

if (-not $SkipBuild) {
    & cmake --build $buildRoot --target PoseidonGame --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "PoseidonGame build failed ($LASTEXITCODE)." }
}
foreach ($path in @($sourceExe, $sourceDll)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing required path: $path" }
}
if (Get-Process -Name 'OpenPoseidon','ColdWarAssault','PoseidonGame' -ErrorAction SilentlyContinue) {
    throw 'The game is running; close it before replacing its executable and renderer DLL.'
}

Copy-Item -LiteralPath $sourceExe -Destination $targetExe -Force
Copy-Item -LiteralPath $sourceDll -Destination $targetDll -Force
foreach ($pair in @(@($sourceExe, $targetExe), @($sourceDll, $targetDll))) {
    if ((Get-FileHash -LiteralPath $pair[0]).Hash -ne (Get-FileHash -LiteralPath $pair[1]).Hash) {
        throw "Deployment hash mismatch: $($pair[1])"
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $fullLogPath) | Out-Null
Remove-Item -LiteralPath $fullLogPath -ErrorAction SilentlyContinue
Push-Location $gameDir
try {
    $gameArguments = @('--check', "--render=$Backend", '--log-file', $fullLogPath, '--log-level', 'info')
    & $targetExe @gameArguments | Out-Null
} finally {
    Pop-Location
}
for ($attempt = 0; $attempt -lt 10 -and -not (Test-Path -LiteralPath $fullLogPath); $attempt++) {
    Start-Sleep -Milliseconds 250
}
if (-not (Test-Path -LiteralPath $fullLogPath)) { throw 'The renderer check produced no log.' }

for ($attempt = 0; $attempt -lt 40; $attempt++) {
    $log = Get-Content -LiteralPath $fullLogPath -Raw
    if ($log.Contains('Initialization check complete - exiting')) { break }
    Start-Sleep -Milliseconds 250
}
foreach ($required in @('Initialization check complete - exiting')) {
    if (-not $log.Contains($required)) { throw "Renderer check missing required log line: $required" }
}
if ($Backend -eq 'wgpu') {
    foreach ($required in @('Wgpu: creating renderer', 'wgpu renderer created')) {
        if (-not $log.Contains($required)) { throw "WGPU check missing required log line: $required" }
    }
    if ($log.Contains('GL33')) { throw 'Explicit WGPU check unexpectedly selected GL33.' }
} elseif (-not $log.Contains('GL33')) {
    throw 'Explicit GL33 check did not select GL33.'
}
Write-Output "Preview-0 $Backend smoke passed: $fullLogPath"

if ($LifecycleSmoke) {
    if ($Backend -ne 'wgpu') { throw '-LifecycleSmoke is a WGPU-only surface lifecycle check.' }

    # This runs the ordinary game loop (rather than the short --screenshot path),
    # minimizes/restores the real SDL window three times, then captures a frame.
    # It is intentionally Windows-only: the Tier-1 Preview-0 contract is Windows.
    $lifecycleLogPath = [IO.Path]::Combine(
        [IO.Path]::GetDirectoryName($fullLogPath),
        ([IO.Path]::GetFileNameWithoutExtension($fullLogPath) + '-normal-loop-minimize-restore.log'))
    $lifecyclePngPath = [IO.Path]::ChangeExtension($lifecycleLogPath, '.png')
    Remove-Item -LiteralPath $lifecycleLogPath, $lifecyclePngPath -ErrorAction SilentlyContinue
    if (-not ('Preview0LifecycleWindow' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Preview0LifecycleWindow {
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int X, int Y, int cx, int cy, uint uFlags);
}
'@
    }
    $lifecycleArgs = "--render=wgpu --window --width=1280 --height=720 --auto-screenshot `"900:$lifecyclePngPath`" --log-file `"$lifecycleLogPath`" --log-level debug"
    $process = Start-Process -FilePath $targetExe -ArgumentList $lifecycleArgs -WorkingDirectory $gameDir -PassThru
    $rendererReady = $false
    for ($attempt = 0; $attempt -lt 240 -and -not $process.HasExited; $attempt++) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $lifecycleLogPath) -and
            (Get-Content -LiteralPath $lifecycleLogPath -Raw).Contains('wgpu renderer created')) {
            $rendererReady = $true
            break
        }
    }
    if (-not $rendererReady) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        throw 'WGPU renderer did not become ready for the lifecycle smoke.'
    }
    # Allow startup scripts to leave the progress loop before the OS transitions.
    Start-Sleep -Seconds 11
    $process.Refresh()
    if ($process.HasExited -or $process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw 'Game exited before the lifecycle transitions could be applied.'
    }
    $resizeBefore = [regex]::Matches((Get-Content -LiteralPath $lifecycleLogPath -Raw),
                                     'SDLEventWindow: window resized to').Count
    # Change the real SDL window size before occlusion.  SWP_NOMOVE | SWP_NOZORDER.
    [Preview0LifecycleWindow]::SetWindowPos($process.MainWindowHandle, [IntPtr]::Zero, 0, 0, 1024, 640, 0x0006) | Out-Null
    $resizeObserved = $false
    for ($attempt = 0; $attempt -lt 100 -and -not $process.HasExited; $attempt++) {
        Start-Sleep -Milliseconds 50
        if ([regex]::Matches((Get-Content -LiteralPath $lifecycleLogPath -Raw),
                             'SDLEventWindow: window resized to').Count -gt $resizeBefore) {
            $resizeObserved = $true
            break
        }
    }
    if (-not $resizeObserved) { throw 'Game did not consume the resize event during the lifecycle smoke.' }
    for ($transition = 0; $transition -lt 3; $transition++) {
        [Preview0LifecycleWindow]::ShowWindowAsync($process.MainWindowHandle, 6) | Out-Null # SW_MINIMIZE
        # Do not restore until SDL has consumed the minimise event.  A fixed delay
        # can merely queue both transitions during loading, which proves neither
        # occlusion handling nor resume safety.
        $minimizeObserved = $false
        for ($attempt = 0; $attempt -lt 100 -and -not $process.HasExited; $attempt++) {
            Start-Sleep -Milliseconds 50
            if ([regex]::Matches((Get-Content -LiteralPath $lifecycleLogPath -Raw),
                                 'SDLEventWindow: window minimized').Count -ge ($transition + 1)) {
                $minimizeObserved = $true
                break
            }
        }
        if (-not $minimizeObserved) { throw 'Game did not consume the minimise event during the lifecycle smoke.' }
        Start-Sleep -Milliseconds 500 # leave at least one rendered frame occluded
        [Preview0LifecycleWindow]::ShowWindowAsync($process.MainWindowHandle, 9) | Out-Null # SW_RESTORE
        $restoreObserved = $false
        for ($attempt = 0; $attempt -lt 100 -and -not $process.HasExited; $attempt++) {
            Start-Sleep -Milliseconds 50
            if ([regex]::Matches((Get-Content -LiteralPath $lifecycleLogPath -Raw),
                                 'SDLEventWindow: window restored').Count -ge ($transition + 1)) {
                $restoreObserved = $true
                break
            }
        }
        if (-not $restoreObserved) { throw 'Game did not consume the restore event during the lifecycle smoke.' }
        $process.Refresh()
        if ($process.HasExited) { break }
    }
    $process.WaitForExit(90000) | Out-Null
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        throw 'WGPU lifecycle smoke timed out.'
    }
    if ($process.ExitCode -ne 0) { throw "WGPU lifecycle smoke exited $($process.ExitCode)." }
    if (-not (Test-Path -LiteralPath $lifecycleLogPath) -or -not (Test-Path -LiteralPath $lifecyclePngPath)) {
        throw 'WGPU lifecycle smoke did not produce both its log and capture.'
    }
    $lifecycleLog = Get-Content -LiteralPath $lifecycleLogPath -Raw
    foreach ($required in @('wgpu renderer created', 'SDLEventWindow: window resized to', 'SDLEventWindow: window minimized',
                             'SDLEventWindow: window restored', 'Auto-screenshot saved', 'Shutdown complete')) {
        if (-not $lifecycleLog.Contains($required)) { throw "WGPU lifecycle smoke missing log line: $required" }
    }
    if ($lifecycleLog.Contains('Assertion failed')) { throw 'WGPU lifecycle smoke logged an assertion.' }
    Write-Output "Preview-0 WGPU lifecycle smoke passed: $lifecycleLogPath"
}

if ($ProfileOverlaySmoke) {
    if ($Backend -ne 'wgpu') { throw '-ProfileOverlaySmoke is a WGPU-only diagnostic UI check.' }

    # Open the real dev panel with Ctrl+F8, select Profile in its fixed Tier-1
    # window layout, and capture the live renderer/capability readout.  The
    # capture is intentionally retained as visual evidence rather than relying
    # on a requested command-line backend string.
    $profileLogPath = [IO.Path]::Combine(
        [IO.Path]::GetDirectoryName($fullLogPath),
        ([IO.Path]::GetFileNameWithoutExtension($fullLogPath) + '-profile-overlay.log'))
    $profilePngPath = [IO.Path]::ChangeExtension($profileLogPath, '.png')
    Remove-Item -LiteralPath $profileLogPath, $profilePngPath -ErrorAction SilentlyContinue
    if (-not ('Preview0ProfileOverlayInput' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Preview0ProfileOverlayInput {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
'@
    }
    $profileArgs = "--render=wgpu --window --width=1280 --height=720 --dev --auto-keys 100:65:192 --auto-screenshot `"1100:$profilePngPath`" --log-file `"$profileLogPath`" --log-level info"
    $process = Start-Process -FilePath $targetExe -ArgumentList $profileArgs -WorkingDirectory $gameDir -PassThru
    $overlayReady = $false
    for ($attempt = 0; $attempt -lt 500 -and -not $process.HasExited; $attempt++) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $profileLogPath) -and
            (Get-Content -LiteralPath $profileLogPath -Raw).Contains('Auto-key injected: frame=100')) {
            $overlayReady = $true
            break
        }
    }
    if (-not $overlayReady) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        throw 'The dev-panel shortcut was not injected during the Profile-overlay smoke.'
    }
    Start-Sleep -Milliseconds 700
    $process.Refresh()
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No game window was available for Profile-tab selection.' }
    [Preview0ProfileOverlayInput]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
    # Profile is the seventh tab in the fixed 1280x720 Tier-1 panel layout.
    $profileTab = New-Object Preview0ProfileOverlayInput+POINT
    $profileTab.X = 385
    $profileTab.Y = 97
    [Preview0ProfileOverlayInput]::ClientToScreen($process.MainWindowHandle, [ref]$profileTab) | Out-Null
    [Preview0ProfileOverlayInput]::SetCursorPos($profileTab.X, $profileTab.Y) | Out-Null
    Start-Sleep -Milliseconds 100
    [Preview0ProfileOverlayInput]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) # MOUSEEVENTF_LEFTDOWN
    [Preview0ProfileOverlayInput]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) # MOUSEEVENTF_LEFTUP

    $process.WaitForExit(90000) | Out-Null
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        throw 'WGPU Profile-overlay smoke timed out.'
    }
    if ($process.ExitCode -ne 0) { throw "WGPU Profile-overlay smoke exited $($process.ExitCode)." }
    if (-not (Test-Path -LiteralPath $profileLogPath) -or -not (Test-Path -LiteralPath $profilePngPath)) {
        throw 'WGPU Profile-overlay smoke did not produce both its log and capture.'
    }
    $profileLog = Get-Content -LiteralPath $profileLogPath -Raw
    foreach ($required in @('wgpu renderer created', 'Auto-key injected: frame=100', 'Auto-screenshot saved')) {
        if (-not $profileLog.Contains($required)) { throw "WGPU Profile-overlay smoke missing log line: $required" }
    }
    Write-Output "Preview-0 WGPU Profile-overlay smoke passed: $profilePngPath"
}

if ($MissionCaptureSmoke) {
    if ($Backend -ne 'wgpu') { throw '-MissionCaptureSmoke is a WGPU-only mission evidence check.' }

    # This is intentionally the original training mission from the installed
    # game.  --check stages it in TempDir and bounds startup; screenshot mode
    # defers that normal early exit until gameplay has rendered its evidence.
    $missionPath = Join-Path $gameDir 'Remastered\Campaigns\1985\missions\00training.abel'
    if (-not (Test-Path -LiteralPath $missionPath)) { throw "Missing training mission: $missionPath" }
    $missionLogPath = [IO.Path]::Combine(
        [IO.Path]::GetDirectoryName($fullLogPath),
        ([IO.Path]::GetFileNameWithoutExtension($fullLogPath) + '-original-training-capture.log'))
    $missionPngPath = [IO.Path]::ChangeExtension($missionLogPath, '.png')
    $missionMetricsPath = [IO.Path]::ChangeExtension($missionLogPath, '.json')
    Remove-Item -LiteralPath $missionLogPath, $missionPngPath, $missionMetricsPath -ErrorAction SilentlyContinue
    $missionArgs = "--check --render=wgpu --test-mission `"$missionPath`" --test-type screenshot --screenshot `"$missionPngPath`" --capture-metrics `"$missionMetricsPath`" --screenshot-delay 10 --log-file `"$missionLogPath`" --log-level info"
    $process = Start-Process -FilePath $targetExe -ArgumentList $missionArgs -WorkingDirectory $gameDir -PassThru
    $process.WaitForExit(90000) | Out-Null
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        throw 'WGPU original-training capture smoke timed out.'
    }
    if ($process.ExitCode -ne 0) { throw "WGPU original-training capture smoke exited $($process.ExitCode)." }
    foreach ($path in @($missionLogPath, $missionPngPath, $missionMetricsPath)) {
        if (-not (Test-Path -LiteralPath $path)) { throw "WGPU mission capture missing artifact: $path" }
    }
    $missionLog = Get-Content -LiteralPath $missionLogPath -Raw
    foreach ($required in @('wgpu renderer created', 'Mission smoke check: mission runtime entered',
                             'Mission smoke check: deferring exit for requested screenshot',
                             'Screenshot test: saved', 'AUTO-TEST SUCCESS')) {
        if (-not $missionLog.Contains($required)) { throw "WGPU mission capture missing log line: $required" }
    }
    $metrics = Get-Content -LiteralPath $missionMetricsPath -Raw | ConvertFrom-Json
    if ($metrics.renderer -ne 'WGPU (Rust / wgpu)' -or -not $metrics.gpu_timestamps_available -or
        $metrics.grass.near_instances -le 0) {
        throw 'WGPU mission capture did not report the required renderer, GPU timings, and populated grass.'
    }
    Write-Output "Preview-0 WGPU original-training capture smoke passed: $missionPngPath"
}
exit 0
