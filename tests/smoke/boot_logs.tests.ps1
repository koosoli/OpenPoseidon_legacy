#!/usr/bin/env pwsh
# Pester v5 startup smoke tests via JSONL log assertions
# Run: Invoke-Pester -Container (New-PesterContainer -Path tests/smoke/boot_logs.tests.ps1 -Data @{ Preset = 'linux-x64-clang-rwdi' }) -Output Detailed
# Tests startup --check mode and verifies key log entries appear.

param(
    [Parameter(Mandatory)]
    [string]$Preset
)

BeforeAll {
    Import-Module "$PSScriptRoot/../cli/TestHelpers.psm1" -Force

    $script:repoRoot = Get-RepoRoot $PSScriptRoot
    $script:gameDir  = Get-GameDataDir -RepoRoot $script:repoRoot
    $script:gameExe  = Get-GameExe -RepoRoot $script:repoRoot -Preset $Preset -Name PoseidonGame
    $script:serverExe = Get-GameExe -RepoRoot $script:repoRoot -Preset $Preset -Name PoseidonServer
    $script:hasAddons = $script:gameDir -and (Test-HasAddons $script:gameDir)
    $script:SkipGame = if (-not $script:gameExe) { "PoseidonGame not found" }
                       elseif (-not $script:hasAddons) { "dist/Game/Addons not found" }
                       else { "" }
    $script:SkipServer = if (-not $script:serverExe) { "PoseidonServer not found" } else { "" }
}

Describe "Game startup checks" {
    BeforeEach {
        if ($script:SkipGame -ne "") { Set-ItResult -Skipped -Because $script:SkipGame }
    }

    It "logs GamePaths directories at boot" {
        $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render dummy
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        Find-LogMessage $logs 'user_dir:'  | Should -Not -BeNullOrEmpty -Because "startup should log user_dir path"
        Find-LogMessage $logs 'cache_dir:' | Should -Not -BeNullOrEmpty -Because "startup should log cache_dir path"
        Find-LogMessage $logs 'temp_dir:'  | Should -Not -BeNullOrEmpty -Because "startup should log temp_dir path"
    }

    It "ephemeral paths: all three dirs resolve to temp" {
        $eph = New-EphemeralGamePaths
        try {
            $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render dummy
            $LASTEXITCODE | Should -Be 0
            $logs = Get-JsonlLogs $output

            $userLog  = Find-LogMessage $logs 'user_dir:'
            $cacheLog = Find-LogMessage $logs 'cache_dir:'
            $tempLog  = Find-LogMessage $logs 'temp_dir:'

            $userLog.msg  | Should -Match ([regex]::Escape($eph.UserDir))  -Because "user_dir should match ephemeral"
            $cacheLog.msg | Should -Match ([regex]::Escape($eph.CacheDir)) -Because "cache_dir should match ephemeral"
            $tempLog.msg  | Should -Match ([regex]::Escape($eph.TempDir))  -Because "temp_dir should match ephemeral"
        } finally {
            Remove-EphemeralGamePaths $eph
        }
    }

    It "logs app tag at boot" {
        $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render dummy
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        $logs.Count | Should -BeGreaterThan 0
        $logs[0].app | Should -Match '^app-[0-9a-f]{4}$' -Because "app tag should be auto-generated"
    }

    It "initializes ScenePreloader during startup check" {
        $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render dummy
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        Find-LogMessage $logs 'ScenePreloader initialized' | Should -Not -BeNullOrEmpty `
            -Because "platform-specific startup should run InitializeSubsystems and populate scene preloads"
    }

    It "initializes all major game startup systems in check mode" {
        $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render dummy
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        Find-LogMessage $logs 'Command-line parsing complete' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Game configuration initialized' -Category 'Config' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'LoadDisplayConfig: applied explicit CLI display overrides' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'World initialized successfully' | Should -Not -BeNullOrEmpty
        ($logs | Where-Object { $_.cat -eq 'Audio' -and $_.msg -like 'Init OK:*' }).Count | Should -BeGreaterThan 0 `
            -Because "audio backend initialization should complete during game startup"
        Find-LogMessage $logs 'FontSystem initialized' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'ScenePreloader initialized' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Initialization check complete - exiting' | Should -Not -BeNullOrEmpty
    }
}

Describe "OpenGL render startup checks" {
    BeforeEach {
        if ($script:SkipGame -ne "") { Set-ItResult -Skipped -Because $script:SkipGame }
    }

    It "starts with GL33 render in check mode" {
        # Was D3D11 before the backend pruning; GL33, WGPU and Dummy
        # remain.  --render gl33 is the explicit form.  The bare default
        # has been `wgpu` since 2026-08-06 (AppConfig.hpp `_renderBackend`),
        # so this test covers the explicit flag only -- it is NOT also a
        # default-path smoke test, which is what this comment claimed until
        # 2026-08-31.
        $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render gl33
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output
        $logs.Count | Should -BeGreaterThan 0
    }

    It "compiles all GL33 shader programs without link errors" {
        # Regression: members of an anonymous uniform block enter the global uniform
        # namespace, so the vsTransform local-light array and the psWater scalar must
        # not share a name. When both were 'lightDir' (vec4[8] vs vec4), the 4
        # VSTransform x PSWater programs failed to link and water silently fell back
        # to PSFlat (DoSelectPixelShader) — losing its bump-specular light_disc glint.
        # Ephemeral cache forces a fresh compile+link so the check is not vacuous
        # (a warm shader cache would skip linking entirely).
        $eph = New-EphemeralGamePaths
        try {
            $output = Invoke-GuiExe $script:gameExe -C $script:gameDir --window --check --log-format jsonl --render gl33
            $LASTEXITCODE | Should -Be 0
            $logs = Get-JsonlLogs $output

            Find-LogMessage $logs 'compiling all programs' | Should -Not -BeNullOrEmpty `
                -Because "ephemeral cache should force a fresh shader compile (else the link check is vacuous)"
            Find-LogMessage $logs 'Program link error' | Should -BeNullOrEmpty `
                -Because "every GL33 program must link; a uniform-name collision across shader stages breaks it"
        } finally {
            Remove-EphemeralGamePaths $eph
        }
    }
}

Describe "Server startup checks" {
    BeforeEach {
        if ($script:SkipServer -ne "") { Set-ItResult -Skipped -Because $script:SkipServer }
    }

    It "logs GamePaths directories at boot" {
        $output = & $script:serverExe -C $script:gameDir --check --log-format jsonl 2>&1
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        Find-LogMessage $logs 'user_dir:'  | Should -Not -BeNullOrEmpty -Because "server startup should log user_dir"
        Find-LogMessage $logs 'cache_dir:' | Should -Not -BeNullOrEmpty -Because "server startup should log cache_dir"
        Find-LogMessage $logs 'temp_dir:'  | Should -Not -BeNullOrEmpty -Because "server startup should log temp_dir"
    }

    It "logs server app tag at boot" {
        $output = & $script:serverExe -C $script:gameDir --check --log-format jsonl 2>&1
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output
        $logs.Count | Should -BeGreaterThan 0
        $logs[0].app | Should -Match '^srv-[0-9a-f]{4}$' -Because "server tag should be auto-generated"
    }

    It "initializes all major server startup systems in check mode" {
        $output = & $script:serverExe -C $script:gameDir --check --log-format jsonl 2>&1
        $LASTEXITCODE | Should -Be 0
        $logs = Get-JsonlLogs $output

        Find-LogMessage $logs 'Command-line parsing complete' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Server configuration initialized' -Category 'Config' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Server sound system initialized' -Category 'Core' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Server world initialized successfully' | Should -Not -BeNullOrEmpty
        Find-LogMessage $logs 'Server initialization check complete - exiting' | Should -Not -BeNullOrEmpty
    }
}
