param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"
Set-Location $RepoRoot

$overlay = "docs/roadmap/modernisation/PoseidonWGPU_Execution_Overlay_Preview0_v1.2.1.yaml"

Write-Host "PoseidonWGPU COMPAT-C1 preflight"

if (-not (Test-Path $overlay)) {
    Write-Host "Canonical Preview-0 overlay is missing. Checking whether HEAD tracks it..."
    git cat-file -e "HEAD:$overlay" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "HEAD does not contain $overlay. Do not activate COMPAT-C1. Restore/migrate Preview-0 history deliberately first."
    }

    Write-Host "Restoring canonical Preview-0 overlay from HEAD..."
    git restore --source=HEAD --worktree -- "$overlay"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $overlay)) {
        throw "Failed to restore canonical Preview-0 overlay."
    }
}
else {
    Write-Host "Canonical Preview-0 overlay exists."
}

# Content, not `git status --porcelain`. On a Windows checkout with
# core.autocrlf=true and `* text=auto`, status keeps reporting ` M` for a file
# whose content is byte-identical to HEAD once line endings are normalised --
# which is what `git restore` above produces. The previous check therefore threw
# on the very state it had just created. `git diff --quiet` applies the same
# normalisation the ledger validators do, so it compares content.
git diff --quiet HEAD -- "$overlay"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Preview-0 overlay differs from HEAD:"
    git diff --stat HEAD -- "$overlay"
    throw "Do not activate COMPAT-C1 while the canonical Preview-0 overlay is modified. Review or restore it first."
}

$validators = @(
    "scripts/validate_preview0_ledger.py",
    "scripts/validate_compat_c1_ledger.py"
)
foreach ($validator in $validators) {
    if (-not (Test-Path $validator)) {
        throw "Missing validator: $validator"
    }
}

# Both, not just Preview-0. The ledger is shared and validated per overlay
# (each record names its owning overlay_id), so Preview-0 passing no longer
# says anything about the COMPAT-C1 records in the same file.
foreach ($validator in $validators) {
    Write-Host "Running $validator ..."
    python "$validator"
    if ($LASTEXITCODE -ne 0) {
        throw "$validator failed. COMPAT-C1 must remain PREPARED."
    }
}

Write-Host ""
Write-Host "PASS: Preview-0 canonical path is intact and both overlay ledgers validate."
Write-Host "Next: confirm the COMPAT-C1 baseline, then activate the overlay atomically."
