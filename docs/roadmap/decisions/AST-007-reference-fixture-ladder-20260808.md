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

> **Corrected 2026-08-08 during AST-015.** The original ladder put the Arma 2
> `ataka.p3d` first on the grounds that all its dependencies resolved locally.
> That was wrong, and the error was mine: the candidate scan that chose it
> required only its *materials* to resolve and never checked its textures.
> Resolving all 12,680 model references through the canonical form showed
> `officialarma` contains **zero** `.paa` files -- the Arma 2 base-game `ca\`
> archives are not in the corpus, only the OA `_e` expansions -- so no Arma 2
> texture resolves. The Arma 3 fixtures resolve completely, including the
> `.tga`->`.paa` substitution. The ladder is reordered accordingly.

Selected by a criterion the overlay does not state but which decides everything:
**the fixture's dependencies must resolve inside the local corpus.** Measured
per prefix rather than as one rate, because "96% resolve" hides which namespace
is missing:

| Resolution of 12,680 model references | Count |
|---|---|
| resolved in a loose source tree | 4,417 |
| resolved in an addon archive | 2,234 |
| unresolved, `ca\data` / `ca\weapons` / `ca\wheeled` / … | 5,980 |
| unresolved, third-party (`vbs2`, `dingo`, `bwmod_wheeled`, `ofp2`) | 37 |
| unresolved, leaked developer host paths (`p:\`, `j:\`) | 3 |

| # | Fixture | Proves | Tickets |
|---|---|---|---|
| 1 | `a3-samples:Addons/Test_Lamp_01/Test_lamp_01_F.p3d` | smallest complete slice: 8 LODs, 2 UV sets, 3 `a3\` RVMATs, `model.cfg` + `config.cpp` present | AST-011A/B/C, AST-015 |
| 2 | `a3-samples:Addons/Test_Plant_F/t_PinusP3s_F.p3d@5beb0501bf7e589d` | vegetation: 10 LODs, 2 UV sets, ShadowVolume + ShadowVolume2, **all 8 references resolve** into `plants_f.pbo` / `data_f.pbo` | grass/tree slice, AST-015 |
| 3 | the RVMATs behind #2 | one real `Super` material end to end | MAT-030, MAT-020 |
| 4 | `a3-samples` PAA set (124 loose files, the only loose PAA in any root) | SWIZ/FLAG/alpha/storage without needing an archive reader | AST-013 |
| 5 | `a3-game` PBOs, ODOL 73 (49/50/52 as the unknown revisions) | safe version dispatch, then one exact revision | AST-012A, AST-012B |
| 6 | `officialarma:…/Air/CA/Air/ataka.p3d@85da76e136e28209` | P3DM geometry across 5 visual LODs + ShadowVolume + Geometry, 2 UV sets; **material resolves, texture does not** | AST-011B (done), AST-018 |

Fixture 6 stays in the ladder because it is a good *geometry* fixture and it is
what AST-011A/B/C were verified against; it is simply not a material or texture
fixture on this machine.

## Missing fixtures — one optional gap

The Arma 3 install was empty when this index was first built; the owner installed
it during AST-007 and the root is now present (487 PBOs, 41 GiB). Every P0 ladder
step is servable.

One gap remains and it is **not blocking**: the **Arma 2 base-game archives** are
absent. `officialarma` ships the Arma 2 Sample Models (loose MLOD sources) and
the OA/expansion PBOs (`cair_e`, `ca\structures_e`, …), but not the base
`ca\data`, `ca\weapons`, `ca\wheeled`, `ca\characters` archives those sources
reference. That is why 5,980 of 12,680 references do not resolve, and why the
corpus contains **zero loose `.paa`** outside `a3-samples`.

1. **What is missing**: the Arma 2 (not OA) base data PBOs providing `ca\data`,
   `ca\weapons`, `ca\wheeled`, `ca\characters`, `cair`, `ca	racked`.
2. **Generation**: Arma 2, base game.
3. **Blocks**: nothing in the C1 P0 set. It would only make the Arma 2 sample
   models usable as *material and texture* fixtures rather than geometry-only.
4. **What it would provide**: cross-generation material coverage — a second
   shader-family corpus to check MAT-030 against, instead of Arma 3 alone.
5. **Where**: `inspiration\officialarma`, alongside the existing packages.

Not requested as work: the Arma 3 fixtures cover every P0 capability, and the
plan's own rule is to pick fixtures a real ticket needs rather than collect them.

The other caveat: the Arma 3 root is indexed by PBO entry table only. Reading an
asset *out* of those archives needs `PackFiles` wired into the resolution path —
still AST-015 follow-on work, not a corpus gap.

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
