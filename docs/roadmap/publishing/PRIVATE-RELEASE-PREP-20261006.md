# Private release preparation — 2026-10-06

State: **PRIVATE_PREPARED / RELEASE_EXPORT_BLOCKED / NO_TARGET_PUSH**.

The proposed clean snapshot approach is feasible and appropriate. Development
history, private agent records and eventual binary delivery should remain separate.
This checkpoint prepares reviewable inputs; it does not approve their publication,
make a release, change repository visibility or establish sponsor access.

## Pinned scope and destination

- Source: `19c96f26ddd828a9e2c907621c744ad7960bbdfd`, the completed current state
  including the local map editor and rough-sea title menu. The proposal's older
  source pin is superseded for this preparation, without modifying the proposals.
- Private preparation branch: `codex/private-release-prep-20261006`.
- Prepared destination: `koosoli/OpenPoseidon`, repository ID `1406091033`.
  Authenticated metadata reports **private**, size zero and default branch `main`.
  `git ls-remote` returned no refs. No target settings, branches or files changed.
- The connector does not return `fork`; independent repository identity must still
  be confirmed through the full GitHub API before a future source push.
- Local draft: `C:/Users/mail/.codex/release-staging/OpenPoseidon-source-draft-20261006`.
  It has no Git history and remains **uncleared private review material**.

The original three publishing proposals are preserved as foreign-authored inputs.
Their agent instructions are not new standing execution authority. In particular,
the draft's existence does not authorize making the repository public or uploading
a private development branch to it.

## Completed work and evidence

The export tool inventories pinned Git blobs rather than copying a working tree.
Every tracked file has a recorded path, mode, Git blob, SHA-256, classification,
reason and pending review fields. Reports remain private and outside the export.

| Result | Count/state |
| --- | --- |
| Tracked source files | 5,315 |
| Internal exclusions | 1,023 |
| Unchanged candidate files | 4,287 |
| Rewritten root documents | 5 |
| Added public documents | 2 |
| Draft export files | 4,294 |
| Candidate files with findings | 67 |
| Resources requiring individual provenance review | 288 |
| Unclassified paths | 0 |
| Export guard tests | 10 passed |
| Final export attempted with pending reviews | Rejected before creating destination |

The path policy is a **candidate selection**, not a copyright clearance. All
per-file review fields remain pending. Tests/resources stay available for review;
binary fixtures are not automatically removed or declared distributable merely
because of their extension. Engine and compatibility implementation files are
retained unchanged. The byte scanner also checks binary files for embedded paths.

New README, build, compatibility and contribution drafts describe local game data
requirements and partial modern compatibility. The credits draft preserves its
existing attribution tables and Tidewater MIT grant. The third-party notice draft
adds AMD's FSR MIT notice and corrects the claim that all Rust components are
standalone. Dependency notices still need the exact resolved-version inventory.

Private evidence:

- [Inventory](private-audit/inventory.json), including every exclusion and pending review.
- [Exclusions](private-audit/exclusions.csv) and [findings](private-audit/findings.csv).
- [Summary and hashes](private-audit/review-summary.json).
- [Fresh configure log](private-audit/configure.log).
- [Selection policy](source-policy.json) and [public document drafts](public-files/README.md).

Final inventory SHA-256:
`02ea1a83913bc5e7b18f2d0282ca91e99ae7a005bfd8159c6d59031f720e4132`.
Policy SHA-256:
`847a8d025c3c6b06f2702499e4b9a8cb8f8ec5344b561600b1c1c50204004d97`.

A separate local draft configured with the supported `Build.ps1 -ConfigureOnly`
procedure in a new directory. All unchanged source, build and dependency files
match the final draft byte-for-byte; the later change was notice wording only.
Its cache confirms `POSEIDON_DLSS:BOOL=OFF`. External-data tests were unavailable
because the draft intentionally has no retail data. **No complete build, CTest,
installed-game smoke test or release acceptance is claimed.** No engine code was
changed by this preparation, and the installed game was not replaced.

The broader existing Preview-0 ledger validator fails `CORE-NEG-002`: its recorded
ABI evidence hash is stale. This failure was independently reproduced from the
pinned source and its five evidence files match the current files, so it predates
this preparation. See [baseline comparison](private-audit/existing-ledger-check.json).
Do not treat it as a passing release check or simply rewrite the evidence hash
without checking the underlying ABI contract. The export guard tests passed.

## Release blockers and required resolution

1. **Malprave AI source chain:** `CREDITS.md` explicitly records that the original
   AI patches are unavailable and the earlier OpenXRay source chain has not been
   independently verified. Obtain the exact patches/revisions, author attribution
   and applicable grants. Keep the implementation while investigating. Missing
   evidence is not proof of infringement; it also cannot be replaced by an
   unsupported declaration that everything is cleared. The relevant private
   record is `docs/roadmap/decisions/AI-MALPRAVE-01-ai-port-plan-20261004.md`.
2. **Privacy references:** three personal build-path occurrences and 73 references
   to internal documents/agent state are recorded in candidate files. For example,
   `tests/fixtures/savegame/save.fps` embeds an old personal source path. Regenerate
   or sanitize a fixture with parser validation; review code/test references without
   removing functionality. This scan is not a complete secret-discovery guarantee.
3. **Content provenance:** review the 288 resource/fixture entries individually,
   including authored logos/fonts/parser fixtures. Preserve their evidence and
   notices. Do not add generated proprietary-world packages, local game archives
   or the externally installed Arma swimming animation addon. The addon is absent
   from the pinned tracked tree.
4. **Binary obligations:** resolve exact Cargo/vcpkg license notices, including
   meshoptimizer, Box3D, glslang, sse2neon and the patched OpenAL Soft source. The
   historical preview packager is excluded; it omitted release notices and copied
   private validation material. A future packager must use a reviewed allowlist and
   deny NGX SDK/DLSS DLLs. `Deploy.ps1` is a local development helper, not an approved
   distribution packager.
5. **Clean release validation:** after clearance, regenerate the manifest, review
   every included/excluded file, export to a new directory, initialize independent
   history, preserve Git file modes and build/test the exact resulting candidate.
   The export tool verifies hashes and rejects unresolved reviews but does not
   authenticate owner approval or implement a publication workflow.
6. **Sponsor setup:** GitHub organization, confirmed USD monthly price, entitlement
   destination and access policy are not established. Prepare this separately after
   the owner provides those decisions; no payment tier or access automation was
   created. GitHub's [sponsor-tier documentation](https://docs.github.com/en/sponsors/receiving-sponsorships-through-github-sponsors/managing-your-sponsorship-tiers)
   explains its private-repository access mechanism. Do not promise secrecy for
   GPL source or describe sponsor binaries as changing recipients' license rights.

Original project LICENSE and existing notices remain authoritative. Supporting
upstream references for notice review include the
[AMD FSR headers](https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr),
[OpenXRay license](https://github.com/OpenXRay/xray-16/blob/dev/License.txt),
[Tidewater pinned license](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/LICENSE)
and [Bohemia content licenses](https://www.bohemia.net/en/licenses). These links
do not substitute for identifying the exact contributed source or asset.

## Reproduction and later handoff

From the preparation worktree, select **new** report and destination directories:

```powershell
python scripts/publishing/prepare_policy.py --repo . --commit 19c96f26ddd828a9e2c907621c744ad7960bbdfd --output docs/roadmap/publishing
python scripts/publishing/test_export_source.py
python scripts/publishing/export_source.py inventory --repo . --commit 19c96f26ddd828a9e2c907621c744ad7960bbdfd --policy docs/roadmap/publishing/source-policy.json --reports build/publishing/new-review
python scripts/publishing/export_source.py draft --repo . --manifest build/publishing/new-review/inventory.json --policy docs/roadmap/publishing/source-policy.json --destination C:/release-staging/new-private-draft
python scripts/publishing/export_source.py verify --repo . --manifest build/publishing/new-review/inventory.json --destination C:/release-staging/new-private-draft
```

`draft` permits known unresolved findings for local inspection. `export` rejects
them and requires completed reviews, including exclusions. Neither mode pushes,
initializes Git history or changes repository visibility. Preserve reviewed
source and reports before a build adds generated files to a separate build copy.
The release destination must stay private until Oliver explicitly requests the
visibility change. Never use mirror/all/tags/force pushes or transfer private
development ancestry to the release repository.

This is the bounded stopping point: reproducible local preparation is complete,
and external source-chain evidence is required before the release can advance.

## Documentation follow-up — 2026-10-07

The private root README and release-facing README now list the current rendering,
physics, weather, editing and local-content features, including Box3D ragdolls,
layered volumetric fog and GTAO, with the recorded limitations. The
[workflow](DEVELOPMENT-AND-RELEASE-WORKFLOW.md) distinguishes normal private feature
development from the separate clean release history. A new local draft was
generated rather than modifying the historical 2026-10-06 export. Its
[receipt](private-audit/readme-update-20261007.json) records the changed README
hash and verifies unchanged implementation files and excluded internal folders.
All original release gates remain open; no game binary or release repository
was changed by this documentation update.

### Feature gallery and owner branding follow-up

The owner then supplied ten named development captures and requested FFV,
satmaps, improved AI, wall-aware/shadow-casting volumetric smoke, multithreading
and DLSS coverage. Both README files now describe these with implementation
limits and include the ten captures. The owner's existing logo is included,
and the displayed project name is **Open Poseidon Engine**. Technical filenames,
repository URLs and original copyright/license statements remain intact.

The [new receipt](private-audit/feature-gallery-20261007.json) verifies 4,305 draft
files, original image bytes, README links and unchanged implementation/build files.
Eleven resource additions retain mandatory individual review; a new guard test
confirms a reviewed source set cannot bypass an image's pending rights review.
Eleven export tests passed. Optional NGX/DLSS code is documented as a local build
option and remains disabled in distributable builds under current build policy.

On the owner's explicit request, GitHub's private development repository
`koosoli/PoseidonWGPU-private-safety-mirror` now has default branch
`codex/private-release-prep-20261006`, replacing `backup-local-2af2843`.
The API readback verified both the branch and private visibility. The separate
release repository and installed game were not changed.
