<#
.SYNOPSIS
    Copy the built game binaries into the retail Arma: Cold War Assault install.
.DESCRIPTION
    Deploys both binaries: OpenPoseidon.exe and wgpu_renderer.dll. Both, always
    - copying only one of them gives an instant "Entry Point Not Found" on launch,
    because the exe and the cdylib share a hand-written C ABI that changes between
    builds. That failure mode is why this is a script and not a Copy-Item you type.

    Staged default content packs and the available DLSS runtime are also copied.
    No original game archive or ColdWarAssault.exe is produced or touched:
    OpenPoseidon.exe is the only binary this project ships. There is deliberately ONE
    name. The folder is shared by several worktrees, and every mismatched-pair incident
    so far started with a second exe in it wearing a name someone recognised.

    Run Install.ps1 once per game install first (it copies BIN, fonts, dtaExt and the
    logo PBO out of the Demo); skipping it degrades silently into a dead Options menu.
.PARAMETER Preset
    CMake preset whose dist/ output to deploy (default win-x64-clang-rwdi).
.PARAMETER GameDir
    Target install. Defaults to the retail install found via the Steam registry.
.PARAMETER PruneStale
    Also delete the legacy PoseidonGame.exe and its PDB, left behind by the old name.
    Off by default - the retail ColdWarAssault.exe is never touched either way.
.EXAMPLE
    .\scripts\Deploy.ps1
.EXAMPLE
    .\scripts\Deploy.ps1 -Preset win-x64-clang-rel -PruneStale
#>
[CmdletBinding()]
param(
    [string]$Preset = 'win-x64-clang-rwdi',
    [string]$GameDir,
    [switch]$PruneStale,
    # Deploy even when this build is an ANCESTOR of what is already installed, i.e. a deliberate
    # roll-back. Without it that case throws; see the DEPLOYED-FROM check below for why.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\CwaCommon.ps1"

if (-not $GameDir)
{
    $GameDir = Get-CwaRetailDir
    if (-not $GameDir) { throw "Retail install (Steam App $CwaRetailAppId) not found in registry. Pass -GameDir." }
}
if (-not (Test-Path -LiteralPath $GameDir)) { throw "Game directory does not exist: $GameDir" }

$repoRoot = Split-Path $PSScriptRoot -Parent
$suffix   = ($Preset -split '-')[-1]
$distDir  = Join-Path $repoRoot "dist\x64-win-$suffix"

# A direct CMake engine build does not regenerate the managed visual PBO.
# Reject stale staged content before any game-folder mutation or new source stamp.
$visualStage = Join-Path $distDir 'Mods/@OP_VisualUpgrade'
if (Test-Path -LiteralPath $visualStage) {
    & "$PSScriptRoot/Assert-DefaultMaterialPackage.ps1" `
        -Archive (Join-Path $visualStage 'AddOns/op_ground_materials.pbo') `
        -ExpectedManifest (Join-Path $repoRoot 'content/default-packs/visual/material-overrides.json') `
        -ToolsExe (Join-Path $distDir 'PoseidonTools.exe') `
        -EvidenceDirectory (Join-Path $repoRoot 'build/deploy-material-preflight') | Out-Null
}

# The two files that must move together. Order is deliberate: the DLL lands first, so
# a half-finished deploy leaves the OLD exe against the NEW dll (which fails loudly at
# startup) rather than a new exe against an old dll (which can fail much later).
$payload = @('wgpu_renderer.dll', 'OpenPoseidon.exe')

# NTFS is case-INSENSITIVE but case-PRESERVING: copying `OpenPoseidon.exe` over a file already
# called `openposeidon.exe` overwrites the bytes and keeps the OLD name on disk. The rename to
# capitals would then be invisible in the game folder forever, and the only symptom is a
# lower-case name nobody can explain. Delete a differently-cased match first so the new casing
# actually lands.
foreach ($name in $payload)
{
    $existing = Get-ChildItem -LiteralPath $GameDir -Filter $name -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -cne $name }
    foreach ($stale in $existing)
    {
        Remove-Item -LiteralPath $stale.FullName -Force
        Write-Host ("  ~ {0} -> {1} (case corrected)" -f $stale.Name, $name)
    }
}

foreach ($name in $payload)
{
    $src = Join-Path $distDir $name
    if (-not (Test-Path -LiteralPath $src))
    {
        throw "Not built: $src`n  Build first: .\scripts\Build.ps1 -Preset $Preset"
    }
}

# WHICH BRANCH IS ALREADY INSTALLED, and is this build actually newer than it?
#
# This check exists because its absence cost a whole session. Several worktrees deploy into this
# one game folder, and `wgpu_renderer.dll` is shared by every exe in it. On 2026-08-22 this script
# copied a dll built from a branch 152 commits BEHIND the one already installed, over the top of a
# matched pair. Nothing failed: the exe still launched, the ABI still matched by luck, and the
# renderer silently lost the road-lighting, texture-streaming and vegetation fixes that lived in
# those 152 commits. The symptoms read as fresh rendering regressions, which is how they were
# reported, and three of them were chased as new bugs before anyone looked at the file dates.
#
# DEPLOYED-FROM.txt is the existing convention for recording this; it was being read by nobody and
# written by nobody but the session that created it. Now it is both.
$stampPath = Join-Path $GameDir 'DEPLOYED-FROM.txt'
$branch = (& git -C $repoRoot rev-parse --abbrev-ref HEAD 2>$null)
$commit = (& git -C $repoRoot rev-parse --short HEAD 2>$null)

# The other half of the staleness problem, and the one the DEPLOYED-FROM check cannot
# see: this build may be current against the GAME FOLDER and still be behind what
# another agent has already published. Deploying it does not lose their commits, but it
# does mean the binary in the folder is not the branch anyone is working from -- and the
# next capture gets attributed to the wrong commit.
#
# A warning, not a refusal: deploying a local build to test it is legitimate and common.
# What is not legitimate is doing it WITHOUT KNOWING, which is the same principle as the
# provenance line further down.
$upstream = (& git -C $repoRoot rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>$null)
if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($upstream))
{
    & git -C $repoRoot fetch --quiet ($upstream -split '/')[0] $branch 2>$null
    $behind = (& git -C $repoRoot rev-list --count "HEAD..$upstream" 2>$null)
    if ($LASTEXITCODE -eq 0 -and [int]$behind -gt 0)
    {
        Write-Warning "This build is $behind commit(s) behind $upstream. Deploying it installs a binary that is NOT the published branch. Rebase first unless you are deliberately testing a local change: git pull --rebase $(($upstream -split '/')[0]) $branch"
    }
}
else
{
    Write-Warning "'$branch' tracks no remote, so nothing can tell you whether it is behind. Run scripts\Use-RepoHooks.ps1."
}
# THE STAMP MUST NOT LIE. It records HEAD, but nothing above checks that dist/ was BUILT from
# HEAD. Two ways it was not, both seen on 2026-09-01:
#  1. dist staging failed silently. CMake copies wgpu_renderer.dll into dist/ after every link;
#     a running game instance holds that DLL, the copy fails, the exe in dist/ keeps its old
#     timestamp -- and the build reports success. Three consecutive "successful" builds left
#     dist/ on a binary two rounds old, and every measurement of them was of code that was
#     not in the binary. The build-tree exe is the ground truth: if it is NEWER than the dist
#     copy, staging did not happen.
#  2. the tree is dirty. HEAD names a commit; the binary may contain uncommitted edits from
#     this or another agent. That is legitimate for a local test but the stamp must say so,
#     or the next capture is attributed to a commit that does not contain what ran.
$buildDir = Join-Path $repoRoot "build\$Preset"
$buildPairs = @{
    'OpenPoseidon.exe'  = Join-Path $buildDir 'apps\cwr\Game\OpenPoseidon.exe'
    'wgpu_renderer.dll' = Join-Path $buildDir 'engine\WgpuRenderer\wgpu_renderer.dll'
}
foreach ($name in $payload)
{
    $built = $buildPairs[$name]
    $staged = Join-Path $distDir $name
    if ((Test-Path -LiteralPath $built) -and (Test-Path -LiteralPath $staged))
    {
        $builtTime  = (Get-Item -LiteralPath $built).LastWriteTime
        $stagedTime = (Get-Item -LiteralPath $staged).LastWriteTime
        if ($builtTime -gt $stagedTime.AddSeconds(2))
        {
            $msg = "$name in dist/ ($stagedTime) is OLDER than the one the build produced ($builtTime): " +
                   "dist staging failed silently -- almost always because a game instance held the DLL. " +
                   "Close the game, rebuild, and deploy the binary you actually built. -Force overrides."
            if (-not $Force) { throw $msg }
            Write-Warning $msg
        }
    }
}
$dirty = (& git -C $repoRoot status --porcelain --untracked-files=no 2>$null | Where-Object { $_ -notmatch 'Cargo\.lock$' })
$dirtyTag = ''
if ($dirty)
{
    $dirtyTag = '+dirty'
    Write-Warning ("Working tree has {0} uncommitted change(s); the binary may contain edits HEAD does not. " +
                   "Stamping as '{1}{2}' so a capture cannot be attributed to a clean commit." -f @($dirty).Count, $commit, $dirtyTag)
}

if (Test-Path -LiteralPath $stampPath)
{
    $prev = (Get-Content -LiteralPath $stampPath -TotalCount 1)
    Write-Host "Installed now: $prev"
    # If the previous stamp names a commit we can resolve, refuse to go backwards without -Force.
    if ($commit -and $prev -match '\b([0-9a-f]{7,40})\b')
    {
        $prevCommit = $matches[1]
        & git -C $repoRoot merge-base --is-ancestor $commit $prevCommit 2>$null
        $isAncestor = ($LASTEXITCODE -eq 0)
        $behind = [int](& git -C $repoRoot rev-list --count "$commit..$prevCommit" 2>$null)
        # STRICT ancestor only. `merge-base --is-ancestor X X` is true for a commit and itself,
        # so redeploying the SAME build -- which is what happens every time you rebuild and push
        # the same commit again, or pass -PruneStale -- would otherwise be refused as a rollback.
        # 0 commits behind is not a rollback; it is the normal case.
        if ($isAncestor -and $behind -gt 0)
        {
            $msg = "This build ($branch $commit) is an ANCESTOR of what is already installed " +
                   "($prevCommit), $behind commits behind. Deploying it would silently REMOVE " +
                   "everything in those commits. Rebase onto the installed branch first, or pass " +
                   "-Force if you genuinely mean to roll back."
            if (-not $Force) { throw $msg }
            Write-Warning $msg
        }
    }
}

$contentPayload = @()
foreach ($name in @('mission.sqm','init.sqs')) {
    $relative = "assets/menu/RoughSea.Intro/$name"
    $source = Join-Path $distDir $relative
    if (!(Test-Path -LiteralPath $source)) { throw "Missing authored menu scene: $source. Run Build.ps1." }
    $contentPayload += @{Source=$source; Relative=$relative}
}
$menuLogo = Join-Path $distDir 'assets/ui/openposeidon_logo.paa'
if (Test-Path -LiteralPath $menuLogo) {
    $contentPayload += @{Source=$menuLogo; Relative='assets/ui/openposeidon_logo.paa'}
}
foreach ($name in @('m16','rifle','ak_rifle','scoped_rifle','smg','machinegun','launcher','pistol','binoculars','nvg','magazine','curved_magazine','grenade','smoke','satchel','mine','rocket')) {
    $relative = 'assets/inventory/' + $name + '.paa'
    $source = Join-Path $distDir $relative
    if (!(Test-Path -LiteralPath $source) -or (Get-Item -LiteralPath $source).Length -eq 0) {
        throw "Missing own inventory icon: $source. Run Build.ps1."
    }
    $contentPayload += @{Source=$source; Relative=$relative}
}
foreach ($pack in @(
    @{Folder='Mods/@OP_VisualUpgrade'; Archive='op_ground_materials.pbo'},
    @{Folder='Mods/@OP_VehicleActions'; Archive='op_jeep_actions.pbo'})) {
    $sourcePack = Join-Path $distDir $pack.Folder
    if (!(Test-Path -LiteralPath $sourcePack)) { continue }
    $metadata = Join-Path $sourcePack 'mod.json'
    $archive = Join-Path $sourcePack ('AddOns/' + $pack.Archive)
    if (!(Test-Path -LiteralPath $metadata) -or !(Test-Path -LiteralPath $archive)) {
        throw "Incomplete default content staging: $sourcePack"
    }
    $manifest = Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if ($manifest.defaultContentApi -cne '1' -or !$manifest.version -or (Get-Item -LiteralPath $archive).Length -eq 0) {
        throw "Invalid default content staging: $sourcePack"
    }
    $existingManifest = Join-Path $GameDir ($pack.Folder + '/mod.json')
    $existingFolder = Join-Path $GameDir $pack.Folder
    if (Test-Path -LiteralPath $existingFolder) {
        if (!(Test-Path -LiteralPath $existingManifest)) { throw "Refusing to overwrite unmanaged folder: $existingFolder" }
        $existing = Get-Content -LiteralPath $existingManifest -Raw | ConvertFrom-Json
        if ($existing.defaultContentApi -cne '1') { throw "Refusing to overwrite unmanaged pack: $existingFolder" }
    }
    $contentPayload += @{Source=$metadata; Relative=($pack.Folder + '/mod.json')}
    $contentPayload += @{Source=$archive; Relative=($pack.Folder + '/AddOns/' + $pack.Archive)}
    $missions = Join-Path $sourcePack 'Missions'
    if (Test-Path -LiteralPath $missions) {
        foreach ($file in Get-ChildItem -LiteralPath $missions -Recurse -File) {
            $contentPayload += @{Source=$file.FullName; Relative=($pack.Folder + '/Missions/' + $file.FullName.Substring($missions.Length+1))}
        }
    }
}

Write-Host "From: $distDir"
Write-Host "To:   $GameDir`n"

if (Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')) {
    & "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir $GameDir
}

foreach ($name in $payload)
{
    $src = Join-Path $distDir $name
    $dst = Join-Path $GameDir $name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $stamp = (Get-Item -LiteralPath $src).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    Write-Host ("  + {0,-22} {1,10:N0} bytes  built {2}" -f $name, (Get-Item -LiteralPath $dst).Length, $stamp)
}

function Get-ContentPayloadHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)) }
    finally { $sha.Dispose(); $stream.Dispose() }
}
foreach ($item in $contentPayload) {
    $destination = Join-Path $GameDir $item.Relative
    New-Item -ItemType Directory -Force (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $item.Source -Destination $destination -Force
    if ((Get-ContentPayloadHash $item.Source) -ne (Get-ContentPayloadHash $destination)) {
        throw "Default content verification failed: $destination"
    }
    Write-Host "  + $($item.Relative)"
}

if ($PruneStale)
{
    foreach ($name in @('PoseidonGame.exe', 'PoseidonGame.pdb'))
    {
        $stale = Join-Path $GameDir $name
        if (Test-Path -LiteralPath $stale)
        {
            Remove-Item -LiteralPath $stale -Force
            Write-Host "  - $name (legacy name, superseded by OpenPoseidon.exe)"
        }
    }
}
else
{
    # Warn rather than delete: an old PoseidonGame.exe next to a new wgpu_renderer.dll
    # is exactly the ABI mismatch above, and someone's desktop shortcut may point at it.
    $stale = Join-Path $GameDir 'PoseidonGame.exe'
    if (Test-Path -LiteralPath $stale)
    {
        Write-Warning "PoseidonGame.exe is still in the game folder. That is the OLD name for this client, it is NOT matched to the wgpu_renderer.dll just deployed, and launching it is the mismatched-pair failure. Re-run with -PruneStale to remove it."
    }
}

# REN-TEMP-001M: a DLSS-featured wgpu_renderer.dll needs the NVIDIA snippet next to the
# exe (NGX searches the executable's folder). Ship the RELEASE snippet whenever the
# deployed renderer was built with the feature; without it DLSS silently falls back to
# the bilinear upscale (FeatureNotFound), which is safe but wastes the build.
$deployedDll = Join-Path $GameDir 'wgpu_renderer.dll'
$snippet = Join-Path $RepoRoot '..\..\..\.tmp-dlss-sdk\lib\Windows_x86_64\rel\nvngx_dlss.dll'
if (-not (Test-Path -LiteralPath $snippet)) { $snippet = Join-Path $RepoRoot '.tmp-dlss-sdk\lib\Windows_x86_64\rel\nvngx_dlss.dll' }
if ((Test-Path -LiteralPath $deployedDll) -and (Test-Path -LiteralPath $snippet))
{
    $dllBytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($deployedDll))
    if ($dllBytes.Contains('NVSDK_NGX'))
    {
        Copy-Item -LiteralPath $snippet -Destination (Join-Path $GameDir 'nvngx_dlss.dll') -Force
        Write-Host "  + nvngx_dlss.dll          (DLSS snippet, release)"
    }
}

# Record what is now installed, in the format the existing file already used:
# "<branch> <commit> <timestamp> <who>". The next deploy READS this, so an unstamped install is
# an install the guard above cannot protect.
$stampLine = "{0} {1}{3} {2} deploy-ps1" -f $branch, $commit, (Get-Date -Format 'yyyy-MM-dd_HH:mm'), $dirtyTag
Set-Content -LiteralPath $stampPath -Value $stampLine -Encoding utf8
Write-Host "Stamped: $stampLine"

Write-Host "`nDone. Launch: $(Join-Path $GameDir 'OpenPoseidon.exe')" -ForegroundColor Green

# ColdWarAssault.exe is the RETAIL binary and this script deliberately never touches it, so it
# only ever gets older. It also sits in the same folder, sorts first alphabetically, and is the
# name anyone who has played the game reaches for -- and it launches and runs, just as whatever
# build it happens to be. On 2026-08-23 that cost a debugging round: a fix was reported as not
# working, from a binary a day and several fixes behind the one just installed.
#
# Named here rather than deleted: it is the retail game's own file and removing it is not this
# script's business.
$retail = Join-Path $GameDir 'ColdWarAssault.exe'
if (Test-Path -LiteralPath $retail)
{
    $retailAge = [int]((Get-Date) - (Get-Item -LiteralPath $retail).LastWriteTime).TotalHours
    if ($retailAge -gt 1)
    {
        Write-Host ("  NOTE: ColdWarAssault.exe in the same folder is {0} h older and is NOT this build. " -f $retailAge) -ForegroundColor Yellow -NoNewline
        Write-Host "Launching it tests nothing you just deployed." -ForegroundColor Yellow
    }
}
