<#
.SYNOPSIS
    Assemble a Preview 0 release package from an explicitly pinned commit.

.DESCRIPTION
    REL-000 requires a "versioned downloadable build or reproducible release
    artifact". Reproducible is the load-bearing word, so this script refuses
    every input that would quietly make the package unreproducible rather than
    warning about it.

    -Commit is mandatory and has no default. That is the whole point. HEAD is
    NOT automatically the release candidate: renderer work lands continuously,
    and defaulting to HEAD is how a package ends up containing whatever happened
    to be committed that afternoon. Naming the commit forces the decision to be
    made by a person, once, on the record.

    Refusals, each corresponding to a failure this project has actually had:

      dirty working tree      A package built from uncommitted changes cannot be
                              reproduced by anyone, including its author.
      HEAD != -Commit         The binaries on disk were built from HEAD. Packaging
                              them while claiming another commit is the stale-binary
                              failure (36a9d29) with a version number attached.
      ledger invalid          The capability matrix and manifest are derived from
                              the ledgers; publishing them while they fail their
                              own validator publishes claims nothing checked.
      missing binary          Both OpenPoseidon.exe and wgpu_renderer.dll, or the
                              package produces Entry Point Not Found on launch.

.PARAMETER Commit
    The commit to package. Required. Must equal HEAD and must be an ancestor of
    the current branch.

.PARAMETER Preset
    CMake preset whose dist/ tree supplies the binaries. Default win-x64-clang-rwdi.

.PARAMETER Version
    Package version label. Default preview0.

.PARAMETER OutputRoot
    Where the package directory is created. Default dist/packages.

.PARAMETER ManifestPath
    Build manifest describing the packaged commit. Defaults to the recorded
    Preview-0 manifest, which normally will NOT match a release candidate: that
    file is TEST-002's evidence and is hashed as such, so a candidate should
    generate its own with write_preview0_manifest.py and pass it here rather than
    overwriting a different ticket's record.

.PARAMETER SkipBuild
    Package what is already staged in dist/ instead of building first. Verified by
    content, never by timestamp -- see the stale-binary note above.

.EXAMPLE
    .\scripts\package_preview0.ps1 -Commit 5b136ca4ba93ca9d026eae2dd8e55c629732a744
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Commit,
    [string]$Preset = 'win-x64-clang-rwdi',
    [string]$Version = 'preview0',
    [string]$OutputRoot,
    [string]$ManifestPath,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot 'dist\packages' }

function Fail([string]$message)
{
    Write-Host "REFUSED: $message" -ForegroundColor Red
    exit 1
}

function Step([string]$message)
{
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Invoke-Native
{
    <#
        Run a native executable and report its exit code honestly.

        Windows PowerShell 5.1 turns any stderr output from a native command into
        an ErrorRecord, and under $ErrorActionPreference = 'Stop' that becomes a
        TERMINATING error -- so `git rev-parse` on a bad revision, or a validator
        printing a diagnostic, aborts the script with a NativeCommandError instead
        of reaching the refusal message written for exactly that case. The exit
        code, which is the actual answer, never gets looked at.

        Dropping to 'Continue' fixes the abort but not the noise: git emits a
        line-ending warning per file it touches, and merging that into the output
        stream renders a dozen red NativeCommandError blocks in the middle of a
        SUCCESSFUL package run. A release tool whose happy path looks like a
        stack of errors trains its user to ignore errors.

        So the process is started directly with its streams redirected to files.
        No ErrorRecords are produced at all, stderr stays available for
        diagnostics, and the exit code is read from the process rather than
        inferred.
    #>
    param([Parameter(Mandatory = $true)][string]$Command,
          [string[]]$Arguments = @())

    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    try
    {
        $startArgs = @{
            FilePath               = $Command
            NoNewWindow            = $true
            Wait                   = $true
            PassThru               = $true
            RedirectStandardOutput = $outFile
            RedirectStandardError  = $errFile
        }
        if ($Arguments.Count -gt 0) { $startArgs.ArgumentList = $Arguments }
        $process = Start-Process @startArgs
        $stdout = (Get-Content -Raw -Path $outFile -ErrorAction SilentlyContinue)
        $stderr = (Get-Content -Raw -Path $errFile -ErrorAction SilentlyContinue)
        return [pscustomobject]@{
            Output = ("$stdout").TrimEnd()
            Error  = ("$stderr").TrimEnd()
            Code   = $process.ExitCode
        }
    }
    finally
    {
        Remove-Item -Force -ErrorAction SilentlyContinue $outFile, $errFile
    }
}

# --- Gate 1: the commit must be real, and it must be the one on disk -----------

Step 'Resolving the pinned commit'
$revision = Invoke-Native -Command 'git' -Arguments @('-C', $repoRoot, 'rev-parse', '--verify', "$Commit^{commit}")
if ($revision.Code -ne 0) { Fail "not a commit: $Commit" }
$resolved = $revision.Output.Trim()

$headResult = Invoke-Native -Command 'git' -Arguments @('-C', $repoRoot, 'rev-parse', 'HEAD')
if ($headResult.Code -ne 0) { Fail "cannot resolve HEAD: $($headResult.Error)" }
$head = $headResult.Output.Trim()

if ($resolved -ne $head)
{
    Fail @"
-Commit ($($resolved.Substring(0,12))) is not HEAD ($($head.Substring(0,12))).

The binaries in dist/ were built from HEAD. Packaging them under a different
commit label would ship a build whose recorded identity is false -- which is the
stale-binary failure this project already hit once, where every check was green
while the deployed file was a day old.

Check out the commit you intend to release, rebuild, and run this again.
"@
}

$statusResult = Invoke-Native -Command 'git' -Arguments @('-C', $repoRoot, 'status', '--porcelain', '--untracked-files=no')
if ($statusResult.Code -ne 0) { Fail "git status failed: $($statusResult.Error)" }
if ($statusResult.Output)
{
    Fail "working tree has uncommitted changes; a package built from it cannot be reproduced.`n$($statusResult.Output)"
}

# --- Gate 2: the ledgers must validate ----------------------------------------

Step 'Validating ledgers and capability matrix'
foreach ($check in @(
        @('scripts/validate_preview0_ledger.py', @()),
        @('scripts/validate_renderer_ledger.py', @()),
        @('scripts/generate_capability_matrix.py', @('--check'))))
{
    $arguments = @((Join-Path $repoRoot $check[0])) + $check[1]
    $result = Invoke-Native -Command 'python' -Arguments $arguments
    if ($result.Output) { Write-Host $result.Output }
    if ($result.Code -ne 0)
    {
        # The validators print their diagnostic to stderr, so it is the useful
        # half of the failure and must be shown rather than swallowed.
        if ($result.Error) { Write-Host $result.Error -ForegroundColor Red }
        Fail "$($check[0]) failed; fix it before publishing anything derived from it."
    }
}

# --- Build --------------------------------------------------------------------

if (-not $SkipBuild)
{
    Step "Building PoseidonGame ($Preset)"
    & (Join-Path $PSScriptRoot 'Build.ps1') -Preset $Preset -Target PoseidonGame
    if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
}
else
{
    Write-Host 'Skipping build; packaging what is staged in dist/.' -ForegroundColor Yellow
}

# --- Gate 3: both binaries, or none -------------------------------------------

Step 'Locating staged binaries'
$distRoot = Join-Path $repoRoot 'dist'
$exe = Get-ChildItem -Path $distRoot -Filter 'OpenPoseidon.exe' -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$dll = Get-ChildItem -Path $distRoot -Filter 'wgpu_renderer.dll' -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $exe) { Fail 'OpenPoseidon.exe not found under dist/. Build first, or drop -SkipBuild.' }
if (-not $dll)
{
    Fail @'
wgpu_renderer.dll not found under dist/, but OpenPoseidon.exe is.

These two are one unit. A package containing only the executable fails at launch
with Entry Point Not Found and no other diagnostic, so this is refused rather
than shipped as a partial package.
'@
}
if ($exe.DirectoryName -ne $dll.DirectoryName)
{
    Write-Host "NOTE: exe and dll come from different directories:`n  $($exe.FullName)`n  $($dll.FullName)" -ForegroundColor Yellow
}

# --- Gate 4: staged binaries must match the build tree, BY CONTENT ------------
#
# 36a9d29: `cmake --build --target PoseidonGame` did not run the wgpu cdylib
# staging step, so dist/ kept whatever the last bare build left -- a day old for
# one binary, twelve days for another, with every build reporting success. A
# stale cdylib does not crash, because the FFI exports still resolve; it presents
# as "the new settings do nothing", which looks like a renderer bug and is not.
#
# Timestamps cannot detect this and neither can hashing against source: both were
# green while the shipped file was a day old. Only content-against-content works.
# Note that an OLDER timestamp on the dll is normal and not evidence of staleness
# -- cargo correctly does not relink when no Rust source changed.

Step 'Verifying staged binaries match the build tree by content'
$buildTree = Join-Path $repoRoot "build\$Preset\apps\cwr\Game"
foreach ($pair in @(@($exe, 'OpenPoseidon.exe'), @($dll, 'wgpu_renderer.dll')))
{
    $staged, $name = $pair[0], $pair[1]
    $built = Join-Path $buildTree $name
    if (-not (Test-Path $built))
    {
        Write-Host "NOTE: no build-tree copy of $name to compare against ($built)" -ForegroundColor Yellow
        continue
    }
    $stagedHash = (Get-FileHash -Algorithm SHA256 -Path $staged.FullName).Hash
    $builtHash = (Get-FileHash -Algorithm SHA256 -Path $built).Hash
    if ($stagedHash -ne $builtHash)
    {
        Fail @"
$name in dist/ does not match the one in build/$Preset.

  staged : $($staged.FullName)
           $stagedHash
  built  : $built
           $builtHash

dist/ is stale. Packaging it would ship a binary that is not the one this commit
builds, which is the failure 36a9d29 exists to prevent -- and it does not crash,
it silently behaves like an older build.
"@
    }
    Write-Host "  $name matches ($($stagedHash.Substring(0,16))...)"
}

# --- Gate 5: the build manifest must describe THIS commit ---------------------
#
# The package ships two documents that each claim to identify it: the Preview-0
# build manifest and the fingerprint written below. Copying the manifest in
# unchecked lets them disagree -- the fingerprint saying one commit and the
# manifest, describing the capture, metrics, adapter and shader hashes, saying
# another. A reader checking the package would find two answers and no way to
# tell which is the build in their hands, which is worse than shipping neither.

Step 'Checking the build manifest describes the pinned commit'
# Defaults to the recorded Preview-0 manifest, but a release candidate normally
# supplies its own with -ManifestPath. That recorded file is TEST-002's evidence
# and is hashed as such, so regenerating it in place to describe a newer build
# would silently invalidate a different ticket's record.
if (-not $ManifestPath) { $ManifestPath = Join-Path $repoRoot 'docs\roadmap\evidence\preview0-manifest.json' }
elseif (-not [IO.Path]::IsPathRooted($ManifestPath)) { $ManifestPath = Join-Path $repoRoot $ManifestPath }
$manifestPath = $ManifestPath
if (-not (Test-Path $manifestPath))
{
    Fail "no build manifest at $manifestPath; generate one with scripts/write_preview0_manifest.py"
}
$manifest = Get-Content -Raw -Path $manifestPath | ConvertFrom-Json
if ($manifest.git_commit -ne $resolved)
{
    Fail @"
The build manifest describes a different commit than the one being packaged.

  packaging : $resolved
  manifest  : $($manifest.git_commit)

The manifest carries the capture, metrics, adapter and shader hashes for the
build it was made from. Shipping it beside a fingerprint naming another commit
gives a reader two identities for one package and no way to choose between them.

Regenerate it against this commit -- scripts/write_preview0_manifest.py, with the
runtime log, capture and metrics produced by this build -- then run this again.
"@
}
if ($manifest.git_dirty)
{
    Fail 'the build manifest records git_dirty: true, so the build it describes cannot be reproduced.'
}

# --- Assemble -----------------------------------------------------------------

$short = $resolved.Substring(0, 12)
$packageName = "cwr-$Version-$short"
$packageDir = Join-Path $OutputRoot $packageName
if (Test-Path $packageDir) { Remove-Item -Recurse -Force $packageDir }
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

Step "Assembling $packageName"
Copy-Item $exe.FullName -Destination $packageDir
Copy-Item $dll.FullName -Destination $packageDir

$docsDir = Join-Path $packageDir 'docs'
New-Item -ItemType Directory -Force -Path $docsDir | Out-Null
foreach ($doc in @(
        'docs/release/preview0/README.md',
        'docs/release/preview0/capability-matrix.md',
        'docs/roadmap/tier1-preview0-validation.md'))
{
    $source = Join-Path $repoRoot $doc
    if (Test-Path $source) { Copy-Item $source -Destination $docsDir }
    else { Write-Host "NOTE: missing expected document $doc" -ForegroundColor Yellow }
}
# The manifest ships under a stable name regardless of which candidate file it
# came from, so a reader always finds it at docs/preview0-manifest.json.
Copy-Item $manifestPath -Destination (Join-Path $docsDir 'preview0-manifest.json')

# --- Fingerprint --------------------------------------------------------------
#
# Hash every file that ships. The recorded identity of a package is the only
# thing a downstream reader can check without trusting us, so it is computed
# from the bytes being shipped rather than copied from the build that produced
# them -- content, not timestamp, is the lesson 36a9d29 left behind.

Step 'Fingerprinting package contents'
$files = Get-ChildItem -Path $packageDir -Recurse -File | Sort-Object FullName
$entries = foreach ($file in $files)
{
    [ordered]@{
        path   = $file.FullName.Substring($packageDir.Length + 1).Replace('\', '/')
        bytes  = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -Path $file.FullName).Hash.ToLower()
    }
}

$fingerprint = [ordered]@{
    schema_version = 1
    package        = $packageName
    version        = $Version
    git_commit     = $resolved
    git_dirty      = $false
    preset         = $Preset
    created_utc    = (Get-Date).ToUniversalTime().ToString('o')
    files          = @($entries)
}
$fingerprintPath = Join-Path $packageDir 'package-fingerprint.json'
# NOT Out-File -Encoding utf8: Windows PowerShell 5.1 writes a BOM, and a leading
# BOM makes the file fail `json.load` in Python and every other strict parser.
# This document exists so an outside reader can verify the package without
# trusting us -- shipping it in a form their tools reject defeats it entirely.
$json = $fingerprint | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($fingerprintPath, $json, (New-Object System.Text.UTF8Encoding($false)))

$zipPath = Join-Path $OutputRoot "$packageName.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zipPath

Step 'Done'
Write-Host "  package : $packageDir"
Write-Host "  archive : $zipPath"
Write-Host "  commit  : $resolved"
Write-Host ''
Write-Host 'Before publishing, confirm the known limitations recorded on REL-000 in' -ForegroundColor Yellow
Write-Host 'docs/roadmap/status-ledger.yaml still describe this build. Measure rather than' -ForegroundColor Yellow
Write-Host 'assume: the last two cost estimates made here were both wrong, one by 30x in' -ForegroundColor Yellow
Write-Host 'the pessimistic direction and one by 4x in the optimistic one.' -ForegroundColor Yellow
