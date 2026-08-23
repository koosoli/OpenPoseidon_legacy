<#
.SYNOPSIS
    Point this checkout (and every worktree of it) at the repo's own git hooks,
    and give the trunk branch an upstream so "am I behind?" has an answer.

.DESCRIPTION
    Run once per clone. Idempotent, so running it again is free.

    WHY THIS EXISTS. Several agents and worktrees share this repository. On
    2026-08-23 the safety mirror carried 29 branches, `feat/consolidated` tracked
    nothing, and no hooks were installed -- so nothing anywhere could tell anyone
    that the commit they had started from was already superseded. The failure mode
    is not losing work, it is having to MERGE it: two sides touching the same files
    for different reasons, reconciled by hand, which is exactly where a change gets
    dropped and nobody notices for a day.

    Hooks live in `scripts/githooks` and are committed, so they arrive with the
    clone. Git will not use them without being told, because `.git/hooks` is not
    version-controlled -- hence `core.hooksPath`, which also applies to every
    worktree of this repository rather than only the one you are standing in.

.PARAMETER Remote
    Which remote the trunk should track. Defaults to `safety`, the private mirror
    that carries the consolidated branch.

.PARAMETER Trunk
    The branch treated as trunk. Defaults to `feat/consolidated`.

.EXAMPLE
    .\scripts\Use-RepoHooks.ps1
#>
[CmdletBinding()]
param(
    [string]$Remote = 'safety',
    [string]$Trunk = 'feat/consolidated'
)

# NOT 'Stop'. Under it, a native command writing anything to stderr becomes a
# terminating NativeCommandError in a non-interactive host -- and git writes
# routine notices there ("LF will be replaced by CRLF..."). This script is meant to
# be run unattended by agents, so it checks $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try
{
    # `core.hooksPath` is repo-relative and shared by every worktree, which is the
    # point: a guard only some worktrees honour is not a guard.
    git config core.hooksPath 'scripts/githooks'
    Write-Host "core.hooksPath -> scripts/githooks" -ForegroundColor Green

    # Windows git ignores the filesystem +x bit, so the mode has to be recorded in
    # the index by hand or a Linux checkout receives non-executable hooks and
    # silently runs none of them.
    foreach ($hook in @('scripts/githooks/pre-commit', 'scripts/githooks/pre-push'))
    {
        if (git ls-files --error-unmatch $hook 2>$null)
        {
            git update-index --chmod=+x $hook 2>$null
        }
    }

    $current = (git rev-parse --abbrev-ref HEAD).Trim()

    $tracked = git rev-parse --abbrev-ref --symbolic-full-name "$Trunk@{u}" 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($tracked))
    {
        git fetch --quiet $Remote $Trunk 2>$null
        git branch --set-upstream-to="$Remote/$Trunk" $Trunk 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0)
        {
            Write-Host "$Trunk now tracks $Remote/$Trunk" -ForegroundColor Green
        }
        else
        {
            Write-Warning "Could not set upstream. By hand: git branch --set-upstream-to=$Remote/$Trunk $Trunk"
        }
    }
    else
    {
        Write-Host "$Trunk already tracks $tracked" -ForegroundColor Green
    }

    # Answer the question once up front, since it costs nothing and it is the same
    # question the hooks will answer later.
    git fetch --quiet $Remote $Trunk 2>$null
    $counts = git rev-list --left-right --count "$Trunk...$Remote/$Trunk" 2>$null
    if ($counts)
    {
        $ahead, $behind = ($counts -split '\s+')
        Write-Host ("`n{0}: {1} ahead, {2} behind {3}/{0}" -f $Trunk, $ahead, $behind, $Remote)
        if ([int]$behind -gt 0)
        {
            Write-Warning "You are BEHIND. Run this BEFORE writing anything: git pull --rebase $Remote $Trunk"
        }
    }

    if ($current -ne $Trunk)
    {
        Write-Host ""
        Write-Warning "This worktree is on '$current', not the trunk '$Trunk'. Fine for a scratch branch; anything meant to ship belongs on $Trunk."
    }

    Write-Host "`nHooks active. A commit onto a stale branch is now refused, with the fix printed." -ForegroundColor Green
}
finally
{
    Pop-Location
}
