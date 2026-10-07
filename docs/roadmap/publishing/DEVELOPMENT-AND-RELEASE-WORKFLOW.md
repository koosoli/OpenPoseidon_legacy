# Private development and clean releases

The branch `codex/private-release-prep-20261006` belongs to the **private
development repository**. It contains the export tools, proposals, review inputs
and private evidence. Its normal Git ancestry, `.agents/`, `.tmp-movecam/` and
`.tmp-shadow/` are expected here. The branch is not the clean release repository.
Do not delete these folders or rewrite private history merely to prepare a release.

At Oliver's explicit request on 2026-10-07, this branch is also the default branch
of `koosoli/PoseidonWGPU-private-safety-mirror`. That selects the private landing
page/default PR base; it does not create a clean release history or change either
repository's visibility. The separate `koosoli/OpenPoseidon` repository is unchanged.

## Further improvements

1. Start each feature/fix branch from the latest agreed, tested private development
   state. Use the established private integration process to combine and validate
   changes. The release-preparation branch is not intended to be the long-term
   gameplay integration branch.
2. Select a tested commit for the next release candidate. Record the full commit
   and its runtime/build evidence. The exporter accepts a commit from any branch
   in this repository; gameplay commits do not have to be merged into the
   preparation branch simply to make them exportable.
3. Update the public document drafts and policy for the chosen feature set.
   Regenerate the inventory from that exact commit and re-review changed files,
   new dependencies, fixtures and export exclusions. Previous approvals or hashes
   are not blanket approval for new code or documents.
4. Export into a **new directory**, validate the exact file set and hashes, and
   build/test an isolated copy of the candidate. Preserve the reviewed source
   snapshot and evidence; do not overwrite an earlier export in place.
5. Once rights, privacy and validation gates are cleared, create the initial
   release history independently. Future source updates go onto that clean
   history as ordinary release commits, preserving notices and recording the
   source pin privately. Never merge private development ancestry into it.

You may normally merge or cherry-pick private preparation-tool/documentation
changes between private branches when needed. There is no requirement to sync
every gameplay feature into this particular preparation branch. There is also
no automatic two-way Git sync between private development and the release repo.
Public fixes should be ported back deliberately, with normal review.

## What goes where

| Location | History and internal files | README |
| --- | --- | --- |
| Private development/preparation branch | Retained | Development overview at root |
| Local candidate export | No `.git`, agents, temporary captures or private roadmap | Curated draft becomes root `README.md` |
| `koosoli/OpenPoseidon` | Independent release history once cleared; no private ancestors | Release-facing feature/status overview |

The public README input lives at `public-files/README.md`; it replaces the private
root README during export. Editing the draft invalidates its old export hash.
Existing exports and audit records are historical snapshots, not live mirrors.
Generate a new candidate rather than assuming an old folder updates itself.

The selection policy excludes `.agents/`, `.tmp-movecam/`, `.tmp-shadow/`, the
private `docs/` tree, old capture material and preparation tooling. Eleven explicitly
selected documentation images (ten owner-provided captures and the project logo)
are separate additions, approved for documentation by the owner on 2026-10-07.
Older binary test/media fixtures without individual provenance are excluded;
see the release-ready receipt and public ASSETS.md for the retained resource basis.
The screenshot directory is not copied wholesale. The per-file
manifest is the authoritative record; merely hiding files with `.gitignore` is
not an export or a history cleanup.

## Current release state

On 2026-10-07 Oliver explicitly authorised a cleaned transfer to the still-private
release repository. The candidate is now on its independent `main` history.
See `SOURCE-CANDIDATE-TRANSFER-20261007.md` for source/target pins and verification.
Privacy findings and the exact clean Windows build/start check have been resolved.
The owner subsequently confirmed Malprave is GPL under the same licence and
same upstream, and that the ten screenshots are his own. The release-ready
follow-up records that confirmation, retains source/component notices and removes
undocumented binary test/media inputs rather than assert their provenance.
Parser/integration tests requiring omitted inputs need authored local fixtures.
The initial candidate records remain historical, superseded by
private-audit/release-ready-20261007.json for the filtered source release. `koosoli/OpenPoseidon` must stay private until Oliver explicitly requests
a visibility change.

For the next candidate, refresh every sanitised source replacement from the new
immutable development pin using `scripts/publishing/prepare_clean_candidate.py`.
Do not reuse old replacement bodies after engine changes: that could silently
export an older implementation. Review the generated diff and curated feature
documentation, commit the policy/replacements privately, then inventory/export
that exact commit anew. Check the candidate Git tree with
`scripts/publishing/verify_candidate_git.py`, build it and repeat the relevant
checks before syncing its reviewed files onto the release repository's existing
independent history. Never merge private development ancestry into `main`.


## Filtered release history

Before filtering the still-private release repository, its exact old main was
backed up to `codex/backup-clean-release-before-fixture-filter-20261007` on the
private safety mirror. The reviewed filtered tree becomes an independent root;
its reachable release history therefore does not retain the omitted fixture
blobs. Replace only the expected old remote main, using an explicit force-with-lease.
Keep the backup in the private mirror, never in a branch/tag of the release repo.
GitHub may retain unreachable objects or cached commit views; rewriting a branch
is not a claim of server-side object erasure. No real rights infringement has been
established for the omitted test data; exclusion is a conservative source release
choice. Do not publish undocumented binary fixtures in future release updates.

This follow-up retains the stable tested development source pin51982. The new
opt-in Earth elevation streaming experiment is on a separate private feature
branch and is not silently included in this release. README, sponsor funding,
resource documentation and test-input exclusions are the only public changes.


## First binary download, 2026-10-08

The owner created private `OpenPoseidon/OpenPoseidon-Builds` and authorised an
initial download. Clean source commit `12f0ab0ffd39792b2c42a3fad7bd87a43b1ff3ec`
now also includes the 15 reviewed code/launcher/test paths of the Earth preview
from private development 51982..6c0075ee, applied as a patch without private
ancestry. This supersedes the earlier stable-only source note above. The build
manifest excludes the separately licensed ImGui Test Engine; binary strings
confirm its removal. Ordinary developer tools remain available.

The first tested 7z includes matched EXE/renderer/audio DLLs, own runtime assets,
default content packs, exact resolved dependency notices and checksums. The
owner explicitly chose a source-commit/download link rather than another engine
source archive: make matching source public before enabling sponsor downloads.
Neither repository visibility nor sponsorship tiers were changed. Installed
Earth-preview binaries remain untouched; the new download was tested separately
against local game data. See private-audit/binary-release-20261008.json.

Future downloads follow a bounded approve/package/test/upload process after a
local build is judged suitable. Upload archives as Release assets; never commit
binaries, copy the entire game directory, or silently replace the matched source
with a link to a different revision. Local asset directories must remain
allowlisted to avoid collecting imported retail resources or scratch images.
