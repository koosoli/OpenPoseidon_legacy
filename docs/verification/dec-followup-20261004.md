# Dec follow-up integration — 2026-10-04

This is a bounded selective source/content integration on
`codex/dec-followup-20261004`, based on `8479c6f3`. The private safety mirror was
fetched again. All 98 previously inventoried tips retain their exact prior hashes
and are ancestors of the base. Two additional tips contain 26 unique incoming
commits:

| New branch | Fetched tip | Decision |
| --- | --- | --- |
| `dec/swim-cave-content-20261004` | `f758e9f3b1008b3bfd1052a0fccaf24a6965e3e1` | Port swimming/falling engine source and independent authored cave test content; hold animation assets and pack activation. |
| `dec/sinkhole-w5-forest-island` | `40e20160ff2ca19fe27d72efb07c093dcabd496e` | Port local conversion tools and 4WVR authored-material support; hold the inherited swimming pack. |

Neither raw tip is merged. Selective patch commits retain source attribution and
identify excluded content, so the integrated branch does not acquire animation
blobs through ancestry. The full inventory, incoming commit/path census, held
19-animation SHA-256 inventory and regeneration checks are in
[the audit receipt](dec-followup-20261004-audit.json).

## Features gained and how to test them

| Port | Feature | Test and limitation |
| --- | --- | --- |
| `64118d6f` | Swimming follows the animated head/chin instead of the chest memory point. Launchers and machine guns reduce stroke travel and float height. | Requires separately supplied compatible animation/config content. Compare rifleman and launcher waterlines and speed in `dev-missions/swim-bay.Eden` / `swim-squad.Eden`. These missions are source-only fixtures; this integration does not provide working swim animations. |
| `64118d6f` | Config-gated falling, hard landing, softer water entry, high water-impact damage, and leaving swim when lifted clear of water. | With no `Fall`/swim moves, the original fall/seabed fallback remains. With suitable external content, test land/water drops and boat exits. Automatic ragdoll ownership retains its earlier return before these authored movement paths. |
| `64118d6f` | Tidewater diagnostic 26 shows the surface's scene/sky/Fresnel interpretation from below. | Set `WGR_TW_DEBUG=26` for an underwater view. This is a diagnostic addition, not a changed water shader appearance. |
| `4e9c2fa2` | Raw 4WVR terrain cells can reference authored RVMAT satellite/mask/surface material stacks. Loose PNG/JPG ground sources can also build the automatic satellite map. | Build or provide a small authored 4WVR fixture and inspect close/far material continuity. Modern OPRW material parsing is factored into the same helper; original OFP PAA/PAC paths remain. |
| `4e9c2fa2` | Such RVMAT-cell worlds retain authored object elevation and hold their materials resident over the whole map. | Verify rocks and trees retain their source placement while moving across small-cell tiles. This applies only to raw 4WVR maps with resolved authored material cells. It is not a correction to the owner's original Everon floating-house report. Full-map residency requires memory/startup timing checks on the intended converted world. |
| `4e9c2fa2` | Local Forest converter handles terrain layers, object LOD groups, nonuniform mesh scaling, winding repair, two-sided foliage, bounded open-rock bedding, and collider-derived geometry/roadway. | Requires an external extraction with terrain, models, scene, material, prefab-root and collider inputs. Run `scripts/forest/Build-ForestIsland.ps1 -Extract <folder>` after building `PoseidonTools`; then use the resulting ignored local mod and `dev-missions/forest-beach.TheForest`. No Forest art, meshes, terrain or extraction is included. Input extraction tools are external and are not provided by this port. |
| `9aabfc2f` | Opt-in underground cave and basement models with terrain-hole memory selections, collision, roadway and AI paths, procedural rock textures, generators, and test missions. | Build `PoseidonTools`, run `scripts/Prepare-SinkholeTestMod.ps1`, and launch `ugcave-test.Eden` or `ugbasement-test.Eden` with the generated mod. The cave mission offers radio commands to test entering/leaving through its tunnel. No default installation hook is added. This provides fixtures to test the already integrated tunnel/path mechanics; it does not certify every AI or Box3D underground interaction. |

## Held content and exact boundary

All 19 files under `content/default-packs/swim/rtm/` are held: `tread`, `swim`,
`swimb`, `swiml`, `swimr`, `swimlb`, `swimrb`, `swimlf`, `swimrf`, `swimturnl`,
`swimturnr`, `swimstart`, `swimstop`, `swimout`, `swimdead`, `swimdeadf`, `fall`,
`fallland`, and `swimin` (1,416,528 bytes at the latest swim tip).

The initial pack record describes Arma 1 poses measured and retargeted onto CWA.
The latest record claims 18 procedurally authored moves but still retains repaired
`swimout`, reads old motion metadata/rifle offsets, and uses stock CWA grip and
landing poses. The generator also requires untracked `G:` helpers and a fitted
joint file. The owner's corrected Arma 1 origin report and these differing claims
do not establish permission to include the resulting binary pack. The pack is
held for that specific decision; this is not a conclusion that the files infringe
copyright or a blanket clearance of other game content.

The swimming config/manifest/README, registration and `--no-swim` additions,
`Build-Swim`, default-build/deployment changes and their matching pack tests are
also held. The new/further-revised swim generator/repair utilities are held with
the asset workflow because their external inputs are unavailable; the earlier
content-free repair script already integrated is unchanged. No default pack is
activated and no missing animations are installed. The previously integrated
source and its content-absent fallback remain available.

## Verification and reproducibility

Performed in the isolated integration worktree, without starting or deploying the
game:

- Python syntax checks passed for all six cave tools and three Forest converters.
- Forest synthetic collision checks passed: eight cube vertices, twelve outward
  hull triangles, planar-input refusal, and P3DM geometry emission.
- Both new PowerShell scripts parsed successfully. The Forest builder additionally
  verifies the exact absolute staging path and refuses a linked staging directory
  before recursive removal.
- `git diff --check` passed. The integrated history has no swim RTM path, and
  default-content registration/build/deployment files have no diff.
- The basement P3D and all four DDS textures regenerate byte-for-byte from the
  included generators. Cave geometry, roadway, paths and fire-geometry LODs are
  byte-identical after decoding. Its visual and memory LODs differ: the supplied
  model hides chamber-hole selections and keeps two open entrances, while the
  generator authors four ordinary hole selections. Full cave regeneration is
  therefore not claimed, and the supplied model is preserved.

Native builds, real GPU composition and installed runtime acceptance are pending
the parent agent's combined integration and deployment. The isolated source
checks above do not claim those tests passed or that the converted Forest scene
is installed on the owner's computer. No public remote is updated.
