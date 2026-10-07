# Clean private source transfer — 2026-10-07

State: `PRIVATE_CANDIDATE_UPLOADED_PUBLIC_RELEASE_BLOCKED`.

Oliver explicitly requested testing a cleaned transfer to his existing
`koosoli/OpenPoseidon` repository. Its visibility remains private and its default
branch is `main`. No GitHub Release, binary upload or visibility change was made.
This supersedes the earlier no-target-push checkpoint, not its outstanding
source-chain/resource review gates.

## Exact source and destination

- Private source: `51982bdacc6df176ffae9b008761b6061b504acf` on
  `codex/private-release-prep-20261006`.
- Independent release root: `266267bd4fb773ecfc4b50780335d7aee389c6c7`.
- Release HEAD: `4168c6eeec4accfb3df56f52393b31ea907ad867`.
- Release tree: `3fb41fed3fcd841520cfa5b346121cb98eb983ec`.
- Target repository ID: `1406091033`; URL: https://github.com/koosoli/OpenPoseidon.
- Reviewed export/build checkout: `C:/dev/openposeidon-release-20261007`.
- Fresh remote readback: `C:/dev/openposeidon-release-readback-20261007`.
- Machine receipt: `private-audit/source-transfer-20261007.json`.

The manifest has 4,339 included files and 1,167 excluded private files. The 54
rewrites comprise five curated root files, comment/example-path privacy edits,
a length-preserving fixture path edit, and five legacy app icons replaced with
the existing project logo. Added files include build/compatibility/release-status
documentation, eleven selected README images, and authored surface expectations
needed by the material tests. Internal-ledger-dependent helpers/plans are excluded.
No runtime engine feature or attribution was removed to pass the privacy scan.

The binary save fixture's null positions, length and non-path bytes are preserved.
Material tests read the exported surface expectations rather than an excluded
internal census. Generated inventory/icon and default-content packaging succeeded.

## Verification

- Thirteen private export/sanitation guard tests passed.
- Thirty-five default-content Python tests passed from the exported tree.
- Full Windows `PoseidonGame` and `PoseidonTools` build succeeded with
  `scripts/Build.ps1 -Preset win-x64-clang-rwdi -Target PoseidonGame --parallel 8`.
  `POSEIDON_DLSS=OFF` was confirmed in the generated cache.
- Generated ground-material, own vehicle-action, menu and 17-icon inventory
  staging succeeded. This does not constitute fresh visual acceptance of each
  gameplay feature.
- WGPU initialization check exited successfully against the owner's installed
  game data, under the shared game lock. The test used the export's binaries;
  installed binaries were not replaced. Its deployment stamp remains `b080696c`.
- A fresh GitHub clone matches every exported blob and mode by SHA-256; one root
  and one documentation follow-up, no private parents, only `main`, and a clean
  working tree. The follow-up adds the required NumPy dependency to BUILDING.md;
  all other file bytes are identical to the successfully built/start-tested root.
- Included-file scan reports no credentials, personal build paths, private repo
  identifiers, agent/internal roadmap references, conflict markers or LFS pointers.
- Complete native test suite, Linux builds and all gameplay/visual regressions
  were not rerun for this source-transfer-only task.

## Public release gates still open

Three manifest findings remain: the recorded Malprave earlier source-chain hold
on `CREDITS.md` and `AIHunt.hpp/.cpp`. Individual resource, fixture and screenshot
publication-rights reviews also remain pending. Existing notices and provenance
are preserved; no pending licence review was falsely changed to passed.

The release repository's `RELEASE_STATUS.md` exposes these limitations. The export
used the honest draft pathway for this authorised private upload; final-public
export remains gated. A binary release additionally needs exact dependency
licences and a reviewed runtime package manifest. Keep all private audit receipts,
logs and manifests outside the release repository.
