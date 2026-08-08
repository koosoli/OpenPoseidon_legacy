# AST-007 — reference corpus inventory and fixture ladder

- **Ticket**: AST-007, overlay `COMPAT-C1-EXEC-001`
- **Date**: 2026-08-08
- **Baseline**: `34c44ac5e4894593b18ea1b47024745d386c5918`
- **Index**: [`docs/roadmap/evidence/compat-c1-fixture-index.json`](../evidence/compat-c1-fixture-index.json)
- **Generator**: [`scripts/build_reference_fixture_index.py`](../../../scripts/build_reference_fixture_index.py)

## What was indexed

6,405 files across the owner-provided roots, parsed to structure rather than
listed. Every P3D LOD was walked to its `#EndOfFile#` tagg and every PBO header
to its entry table, with **zero parse failures** — a desynchronised walk throws
rather than guessing, so a clean run is itself evidence the layout assumptions
hold across both generations.

| Root | State | Parsed | Notes |
|---|---|---|---|
| `a3-samples` | present | 163 | 34 P3D, 124 PAA, 5 RVMAT, 35 RTM, model.cfg + config.cpp per addon |
| `officialarma` | present | 5,697 | 1,627 P3D, 4,070 RVMAT, 3,436 RTM, 58 PBO, 47 BISurf |
| `a3-game` | present | 487 | 487 PBOs, 41 GiB; entry tables only, no loose asset files |

## Findings that change the plan

**Every loose P3D in both corpora is MLOD/P3DM. There is not one SP3X visual
model.** All 34 Arma 3 Samples models and 1,625 of 1,627 `officialarma` models
open with `P3DM`; the only two SP3X files are camera helpers
(`RootCfg/Data/cam_360.p3d`, `cam_zoom_in_front.p3d`). The production reader
handles SP3X only, so **AST-011A is not one format among several — it is the gate
in front of the entire local corpus.** Nothing else can be proven until it lands.

**Multi-UV is the common case, not an edge case.** 626 models carry two or more
`#UVSet#` taggs, including the first fixture examined. AST-011C's
"no UV1→UV0 collapse" rule cannot be deferred behind a "typical" single-UV slice,
because the typical model is not single-UV.

**Five ODOL revisions are available locally, inside the PBOs.** A walk of loose
files reports zero ODOL fixtures and makes AST-012A/012B look unservable. Reading
the 545 PBO entry tables instead finds binarized models at revisions **48, 49, 50,
52 and 73**, each archive declaring its addon prefix (`ca\…` for Arma 2/OA,
`a3\…` for Arma 3). AST-012B's "one exact strategically useful later revision"
can therefore be a real choice rather than the only one available: **73** (Arma 3,
162 PBOs) is the newest and most strategically useful, **49** (Arma 2, ~1,400
models) the widest. Two unrelated revisions are on hand for the "second unknown
revision must reject safely" test without inventing a fixture.

**RVMATs reference `.tga`, shipped textures are `.paa`.** Material stages name
source-side `.tga` paths that never exist on disk. Confirmed against the Arma 3
install: all seven of `t_PinusP3s_F.p3d`'s dependencies resolve, but only after
substituting `.tga`→`.paa` and mapping the prefix onto the owning PBO
(`a3\plants_f` → `plants_f.pbo`, `a3\data_f` → `data_f.pbo`). Resolution
therefore needs a source→shipped substitution *and* a prefix→archive map, not just
prefix normalisation — an AST-015 requirement the overlay does not currently
state.

**`Super` dominates the Arma 2 material corpus**: 2,853 of 4,070 RVMATs, with
`NormalMapMacroASSpecularDIMap` (209) and `NormalMapSpecularDIMap` (191) next.
MAT-030's "first real shader-family schema, preferably Super" is the right call on
the evidence. 3,793 RVMATs declare a `uvSource`, so TexGen is not optional.

**No RVMAT in either corpus is binarized** — all 4,070 are text. AST-014 can defer
binarized-ParamFile material decoding until a fixture demands it.

## The ladder

Selected by a hard criterion the overlay does not state but which decides
everything: **the fixture's dependencies must resolve inside the local corpus.**
The Arma 3 Samples models are the better-documented sources, but every one of
them references `a3\…`, which resolves only through the 487 PBOs of the game
install — an archive read the resolution path does not have yet. The
`officialarma` models reference `ca\…`, and 5,289 of those virtual paths are
satisfied by **loose files in the same tree**, so steps 1–4 need no PBO reader at
all. That is why the first slice is Arma 2 source rather than the better-commented
Arma 3 samples: it removes AST-015 archive work from the critical path without
giving up any capability the slice has to prove.

| # | Fixture | Proves | Tickets |
|---|---|---|---|
| 1 | `officialarma:A2SM_Data_DPL_APL/Air/CA/Air/ataka.p3d@85da76e136e28209` | P3DM dispatch, faces, 5 visual LODs + ShadowVolume + Geometry, 2 UV sets, one `Super` material, one `ca\` texture — all deps resolvable | AST-011A/B/C, AST-015, AST-014, MAT-030 |
| 2 | `ca\air\data\ataka.rvmat` (`Super`, roles: BaseColour, NormalMap, SpecularMapDetail, AmbientShadow) | one real Super material end to end | MAT-030, MAT-020 |
| 3 | its PAA set | SWIZ/FLAG/alpha/storage on a texture the model actually uses | AST-013 |
| 4 | `officialarma:…/Creatures/CA/Characters_E/Heads/Glasses/Delta1G.p3d` (586f, 2 materials, adds BaseColourAlpha) | alpha policy and multi-material on the same path | AST-013, MAT-020 |
| 5 | `a3-game` PBOs, ODOL 73 (dispatch tested against 49/50/52 as the unknown revisions) | safe version dispatch, then one exact revision | AST-012A, AST-012B |
| 6 | `a3-samples:Addons/Test_Plant_F/t_PinusP3s_F.p3d@5beb0501bf7e589d` | vegetation: 10 LODs, 2 UV sets, ShadowVolume + ShadowVolume2, 5 materials, all 7 dependencies verified resolvable in `plants_f.pbo` / `data_f.pbo` | grass/tree slice, AST-015 |

Fixture 1 is deliberately small (175 KB, 438-face visual LOD0) and asymmetric
across its five visual LODs, so a winding or UV-collapse error is visible rather
than plausible.

## Missing fixtures — none

The Arma 3 install was empty when this index was first built, which blocked the
vegetation fixture. The owner installed Arma 3 during this ticket and the root is
now present (487 PBOs, 41 GiB). The blocker is resolved and **no further reference
material is needed for the C1 P0 set**:

- ladder steps 1–4 are servable from `officialarma` alone;
- ODOL dispatch and one exact later revision have five revisions to choose from;
- the vegetation fixture's seven `a3\…` dependencies were each located in a
  specific PBO, so step 6 is unblocked.

The one caveat worth recording: the Arma 3 root is indexed by PBO entry table
only. Reading an asset *out* of those archives needs the existing `PackFiles`
reader wired into the resolution path, which is AST-015 work, not a corpus gap.

## Constraints honoured

- No absolute path is tracked. Roots come from gitignored
  `.reference-corpora.json` (see `.reference-corpora.json.example`) or
  `$POSEIDON_REFERENCE_CORPORA`; every root is optional and an absent one is
  recorded, not raised. CI has none of them and the tool still runs.
- Nothing is decoded or redistributed: the index stores paths, hashes, header
  facts and capability tags. No pixel data, geometry or material content leaves
  the owner's licensed installs.
- Fixture IDs are `alias:relative-path@sha256[:16]`, so they are stable across
  machines and a re-downloaded package that differs is a different fixture.
- Full parsing runs only for roots marked `deep`; the game install stays shallow.
