<#
.SYNOPSIS
    Copy the built game binaries into the retail Arma: Cold War Assault install.
.DESCRIPTION
    Deploys exactly two files: PoseidonGame.exe and wgpu_renderer.dll. Both, always
    - copying only one of them gives an instant "Entry Point Not Found" on launch,
    because the exe and the cdylib share a hand-written C ABI that changes between
    builds. That failure mode is why this is a script and not a Copy-Item you type.

    Nothing else is written, and no ColdWarAssault.exe / PoseidonGame.exe is produced
    or touched: PoseidonGame.exe is the only binary this project ships.

    Run Install.ps1 once per game install first (it copies BIN, fonts, dtaExt and the
    logo PBO out of the Demo); skipping it degrades silently into a dead Options menu.
.PARAMETER Preset
    CMake preset whose dist/ output to deploy (default win-x64-clang-rwdi).
.PARAMETER GameDir
    Target install. Defaults to the retail install found via the Steam registry.
.PARAMETER PruneStale
    Also delete openposeidon.exe and its PDB, left behind by the parked rename.
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

# The two files that must move together. Order is deliberate: the DLL lands first, so
# a half-finished deploy leaves the OLD exe against the NEW dll (which fails loudly at
# startup) rather than a new exe against an old dll (which can fail much later).
$payload = @('wgpu_renderer.dll', 'PoseidonGame.exe')

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

Write-Host "From: $distDir"
Write-Host "To:   $GameDir`n"

foreach ($name in $payload)
{
    $src = Join-Path $distDir $name
    $dst = Join-Path $GameDir $name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $stamp = (Get-Item -LiteralPath $src).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    Write-Host ("  + {0,-22} {1,10:N0} bytes  built {2}" -f $name, (Get-Item -LiteralPath $dst).Length, $stamp)
}

if ($PruneStale)
{
    foreach ($name in @('openposeidon.exe', 'openposeidon.pdb'))
    {
        $stale = Join-Path $GameDir $name
        if (Test-Path -LiteralPath $stale)
        {
            Remove-Item -LiteralPath $stale -Force
            Write-Host "  - $name (stale, pre-rename)"
        }
    }
}
else
{
    # Warn rather than delete: an old PoseidonGame.exe next to a new wgpu_renderer.dll
    # is exactly the ABI mismatch above, and someone's desktop shortcut may point at it.
    $stale = Join-Path $GameDir 'openposeidon.exe'
    if (Test-Path -LiteralPath $stale)
    {
        Write-Warning "openposeidon.exe is still in the game folder. It came from the parked rename and is NOT matched to this wgpu_renderer.dll; launching it is the mismatched-pair failure. Re-run with -PruneStale to remove it."
    }
}

# Record what is now installed, in the format the existing file already used:
# "<branch> <commit> <timestamp> <who>". The next deploy READS this, so an unstamped install is
# an install the guard above cannot protect.
$stampLine = "{0} {1} {2} deploy-ps1" -f $branch, $commit, (Get-Date -Format 'yyyy-MM-dd_HH:mm')
Set-Content -LiteralPath $stampPath -Value $stampLine -Encoding utf8
Write-Host "Stamped: $stampLine"

Write-Host "`nDone. Launch: $(Join-Path $GameDir 'PoseidonGame.exe')" -ForegroundColor Green

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
