# Handover — four worlds, four bottlenecks, and what is still open

**Branch:** `feat/texture-streaming` (contains all 25 `feat/farfield-tier` commits plus this
session's). Pushed to `safety`.
**Date:** 2026-08-15

---

## READ THIS FIRST — two traps that cost this session real credibility

1. **`farfield-bench.ps1` defaults to 800×600. The owner plays at 1600×900.** Three times the
   pixels; on a GPU-bound world the GPU frame scales close to linearly. I quoted "~50 fps" on
   Everon from an 800×600 run and was told "are you kidding" — the real figure was 40.5.
   **Always pass `-Width 1600 -Height 900` for any number the owner will read, and state the
   resolution beside every fps figure.** Use 800×600 only to compare arms with each other.

2. **Instance count is not detail.** I shipped `WGR_LOD_GOVERNOR_RANGE=4` on the claim that
   Everon "draws the SAME 39,670 instances, so nothing is lost". True and irrelevant: the LOD
   *distribution* collapsed (LOD-0 population 13,800 → 631) and the owner reported the forest
   as low-poly. **Check the LOD histogram, not `main_instances`.** It is in every capture
   JSON as `objects.main_lod_histogram`.

3. **EVERY TEST WORLD LIVES IN %TEMP%, AND THE DISK IS FULL.** All four worlds
   (`ff-worlds/{stratis,chernarus,sahrani}` ~299 MB and `rf/world/ev_DALL.wrp` 99 MB), plus
   the Reforger `.emat` corpus and every derived census, sit under
   `AppData\Local\Temp`. Windows Storage Sense and Disk Cleanup delete temp directories,
   and a 100%-full drive is exactly what provokes them. **Check the world file exists before
   blaming a run that failed to load one** — and if there is somewhere durable to put them,
   moving them is worth more than any fix in this document. They are hours of export work
   and no single script rebuilds them all.

Also: the owner's machine has **C: 100% full at 953 GB**. Builds fail with a German
"not enough space" error from `cargo`/`ninja` long before anything says "disk".

---

## The four worlds

All settled (`pending=0`), 1600×900, current defaults unless noted.

| world | fps | bound by | status |
|---|---|---|---|
| Reforger Everon | 34.1 | GPU geometry | governor decoupling available, off by default |
| Arma 3 Stratis | — | was Pass1 CPU draw | **fixed** (+159% @ 800×600) |
| DayZ Chernarus | 26.4 | texture VRAM | renders only with `WGR_TEXTURE_MIP_BIAS=1` |
| Arma 1 Sahrani | 20.2 | `drw:land` → Pass2 alpha | diagnosed, not fixed |

Every one contradicted the generalisation from the world measured before it. Do not assume a
fix transfers; measure each.

## What was fixed

- **Stratis**: a whole-model TreeAdv bail rejected 88,084 of 163,953 placements at
  *registration*, so the CPU drew half the world. Lifting it took `land:obj` 42.702 → 0.185 ms.
  `WGR_TREEADV_RETAINED` (default **on**). Its own comment said to remove it once retained
  alpha-to-coverage landed; that had landed and nobody went back.
- **Chernarus**: renders at all for the first time. The texture LRU was **starved**, not badly
  tuned — 113 rounds finding 1–31 candidates against 3,552 textures, because
  `RetainGpuModel` pins every texture of every registered model.
- **Texture upload**: one VFS open per texture instead of one per mip level; alpha
  classification read off the compressed blocks (−54% on the hit path).
- **Instrumentation** — this is what turned three vague "it's slow" reports into named lines:
  `WGR_PASS1_STATS` (splits `land:obj` six ways + the coverage split + Pass2 five ways),
  `WGR_LAND_STATS` (splits `drw:land`), `WGR_TEXTURE_STREAM_STATS`.

## Open, in the order I would take them

1. **Arma 1: 778 alpha objects at 36 µs each** = 27.9 ms of a 47 ms frame. They sit in the
   `none=937` coverage bucket while 97.3% of the world is diverted. **The same 778 are the
   missing-shadow bug** — CPU-drawn objects go through main-view caster selection and a 250 m
   `shadowsZ` clamp, hence "shadows pop in depending on viewing angle". One fix, two symptoms.
   Template: the Stratis bail above.
2. **The streaming transient** — what the owner sees on every launch. `admitBudgetSpent`
   cannot consult its 4 ms budget until the floor (128) has been *attempted*, so a batch of
   cold models costs whatever it costs. `WGR_OBJECT_STREAM_MIN_BATCH=1` halves the stutter
   (172 → 92 ms median) and lengthens the fill 39% (63 → 88 s). **Neither is playable**: at
   floor 1 a *single* cold create is ~90 ms, because the ODOL decode, LOD conversion, GPU
   registration and texture transcode are all synchronous. The real fix is making that
   asynchronous.
3. **Reforger materials.** 100% of Everon materials are `.emat` and *none* opens: the section
   keeps the raw Enfusion path while its textures were rewritten to the deployed root. Even
   if found, `TranslateEmat` reads DayZ keys (`AlbedoMap`/`NormalMap`) — of 10,312 real
   Reforger `.emat` files only **2.2%** name any key it reads; the real ones are `BCRMap`,
   `NMOMap`, `MaskMap`. **Zero normals and zero speculars are bound anywhere on Everon.**
4. **Reforger ground**: `PakCommand.cpp` hardcodes a 4096 satellite for a 12.8 km world =
   3.125 m/texel (Stratis ~1.03). It is not imagery — it is the area-averaged mean colour of
   5 material slots, with 17 of Everon's 22 surfaces folded into the nearest by colour.
   Raising the resolution sharpens boundaries between flat colours and adds no detail.
5. **Chernarus texture pinning** — the real fix behind the mip-bias workaround. A pin should
   mean "recently drawn", not "registered". **It cannot be done from C++**: `register_model`
   bakes each texture's *bindless slot index* into the retained material table, there is no
   unregister, and `SharedTextures::destroy` returns the slot to a LIFO free list, so the next
   upload claims it and the live model silently samples an unrelated texture. Needs Rust-side
   **stable slots** (reserve a slot per texture identity) or a slot indirection table.

## Levers added this session (all default-off unless stated)

| lever | what it does |
|---|---|
| `WGR_TREEADV_RETAINED` | **default ON** — foliage on the retained path (the Stratis fix) |
| `WGR_LOD_GOVERNOR_TARGET=1` | governor drives on the target point, not a perpendicular distance — makes the range a clamp again. +36% on Everon *with* a real LOD ladder |
| `WGR_TEXTURE_MIP_BIAS=1` | drops top mip levels. **Costs texture resolution.** The only reason Chernarus renders |
| `WGR_OBJECT_STREAM_MIN_BATCH` | admission floor; 1 halves fill-in stutter, lengthens the fill |
| `WGR_WATER_SHORE_FOAM_LEGACY=1` | restores the old coastline foam |
| `WGR_SHADOW_RECOVER_LOD=1` | recovers a shadow LOD for objects the software occlusion zeroed |
| `WGR_PASS1_STATS` / `WGR_LAND_STATS` / `WGR_TEXTURE_STREAM_STATS` | the splits above |
| `WGR_STAGING_STRICT=1` | restores the panic on a failed staging allocation (now degrades) |
| `WGR_GEO_POOL_GROWTH` | geometry pool growth factor, default 2.0 |

## Things that are true and non-obvious

- **`shadowCasters=0` proves nothing.** `ShadowMapsEnabled()` defaults **true** on wgpu, so
  the projected accumulator never runs. The "off by default" comments are stale; I wasted a
  measurement on them.
- **`geometryRetired=0` is not an eviction failure.** It is a per-frame transient emptied
  after every submit, so it is zero at any steady-state sample by construction.
- **`registered=N` is retained *instances*,** a different denominator from resident objects.
- Pass1 is **linear** in the object set (~3 µs/object). An earlier claim of superlinearity
  came from comparing two governor arms that differ in LOD as well as count.
- The **fill-in is ~107 s on Everon at 1600×900** and 253 s on Chernarus. "I launched it and
  it was under 10 fps" is a report about the fill-in, not the settled frame. Both are true.

## Reported by the owner, diagnosed, not yet fixed

- **Arma 2 roads through mountains** — the decal depth bias is a *constant* NDC offset, but
  reversed-Z depth is `near/z`, so it pulls a road ~95 m forward at 1 km and 716 m at 3 km.
  Fixed in `shader3d.wgsl` (scale by ndc²) — **deployed but not visually verified.**
- **DayZ rails "piano"** — `ObjectClasses.cpp:757` bails on any road model with a geometry
  LOD, skipping the `ClipLandOn` fix at `:795`. Rail ballast is collidable, so it has one.
  Likely also the residual Arma 2 road banding, and cheaper to check than WLD-021's standing
  CSM hypothesis.
- **"Drunk" swimming normals** — parallax occlusion mapping exists only on the per-draw path,
  and roads are excluded from the retained set, so roads get it and buildings do not.
- **Arma 3 light orbs** — pre-existing, reproducible at freefly `2736.50, 5993.66, 45.19`
  az 4.5 el −27.1, and visible in `.tmp-bench/foam-new/run-01.png`.
- **Grass self-shadow toggle dead** — plumbing is intact end to end, but `shadow_strength` is
  only called from `fs_grass_mid_tuft`, bound only when `midPhotoTuft` (default false). Grass
  casts into the CSM but `fs_grass`/`fs_grass_cards`/`fs_grass_far` sample no cascade at all.

## How to work here

Subagents with **disjoint file ownership** and an explicit list of who else is editing. Ask
for null results in the brief — the most valuable findings this session were agents refusing
the premise they were given: the texture-pin agent proved the fix I asked for is impossible
from C++, and the streaming agent killed three of my four suspects and my "1.1 ms per object"
figure. Both were worth more than the change I asked for.

---

# Working overnight, unattended

The owner is asleep. Nobody will answer a question, so **do not stop to ask one** — pick the
interpretation you can defend, write down which one you picked and why, and keep going. A
blocked agent that waited eight hours is worse than one that made a defensible call and
recorded it.

## Push after every milestone — this is the recoverable point

```
git push safety feat/texture-streaming
```

`safety` is `https://github.com/koosoli/PoseidonWGPU-private-safety-mirror`. It is a **private
mirror**, not the public repo — push there freely and often. Work on `feat/texture-streaming`
(it already contains all of `feat/farfield-tier`). The previous session pushed 18 times and
every push was a recoverable point; this one pushed ~12. Do not let work sit uncommitted
overnight: an unattended crash with four hours of uncommitted work is the one genuinely
unrecoverable outcome here.

Commit messages on this branch carry the measurement and the reasoning, not just the change.
Match that. If a number is in the message it must come from a run, not an estimate.

## The rules that actually produced results

1. **Measure before and after. Quote the resolution.** `-Width 1600 -Height 900`.
2. **Split the phase before optimising it.** Every real win this session came from turning one
   number into six: `land:obj` → submission, `drw:land` → Pass2, Pass2 → alphaDraw. Guessing
   which part was expensive was wrong every single time.
3. **Env-gate behavioural changes, safe behaviour as default.** Then A/B from one binary.
4. **A default is a claim about every world.** Two of this session's three worst mistakes were
   shipping a default validated on one world.
5. **Subagents with disjoint file ownership**, told who else is editing. Ask for null results
   explicitly — the best findings were agents refusing the premise they were handed.
6. **Do not run the game and build at the same time** if a benchmark matters; and never trust
   a frame number from a run where another game process was resident.

## Ordered work list for the night

Everything below is diagnosed to a file and line in this document or in `docs/roadmap/decisions/`.

1. **Arma 1's 778 alpha objects** — 27.9 ms of a 47 ms frame, *and* the missing-shadow bug.
   The binary already logs their names under `WGR_PASS1_STATS=1`; one Sahrani run identifies
   them and picks between the two fixes.
2. **Verify the three deployed-but-unseen fixes**: road depth bias (needs a road behind a
   ridge at ≥1 km), shadow LOD recovery, foam at a second pose.
3. **`WGR_LOD_GOVERNOR_TARGET=1` on Stratis, Chernarus and Sahrani.** It is +36% on Everon
   with a proper LOD ladder and is off only because one world is not enough to justify a
   default. Three arms and it can ship.
4. **DayZ rails / Arma 2 roads** — `ObjectClasses.cpp:757` bails on any road model with a
   geometry LOD, skipping the `ClipLandOn` fix at `:795`. Confirm with `--log-level debug` and
   grep `Road with geometry`.
5. **The streaming transient** — the only real fix is asynchronous model loading; the floor
   lever is a palliative.

# Review of docs/roadmap/modernisation — read this before working from it

**The ledger is stale, and the roadmap is 253 KB. Do not treat either as a to-do list without
checking the code first.** `docs/roadmap/status-ledger.yaml` has 61 tickets: 36 VALIDATED,
15 INTEGRATED, **9 PLANNED**, 1 RESEARCHED. Several of the PLANNED ones are substantially
done and were never re-statused.

**Marked PLANNED but demonstrably working** (verified by loading the worlds this session, and
by decision records that postdate the ticket):

- **AST-012 "Later ODOL compatibility"** — has its own sub-decisions AST-012B (ODOL 73),
  AST-012C, AST-012D (ODOL 40). Arma 1 (rev 40), Arma 2/OA, DayZ (rev 54) and Reforger models
  all load and render. This is not a PLANNED ticket.
- **AST-022 "WRP/OPRW world-format expansion"** — `Oprw18/20/24/25` readers exist and
  WLD-010 covers the OPRW-25 container. This session booted `sara.wrp` (OPRW 20, Arma 1),
  `stratis.wrp`, `chernarusplus.wrp` and `ev_DALL.wrp`. Four generations, four worlds.
- **AST-016/017 (model.cfg / RTM animation)** — has a scoping decision, AST-016-017. Check it
  before assuming the ticket text is current.

**Genuinely open, and worth doing:**

- **MAT-046 (RESEARCHED)** — "the Arma 2 and Arma 1 section readers drop the word that hides a
  proxy". This is the only RESEARCHED ticket and it sits directly on top of the owner's Arma 2
  road complaints. Read it early; it may be the same root cause.
- **AST-023 automated cross-generation conformance harness** — ad-hoc harnesses exist per
  generation (see the memory notes) but nothing automated ties them together. This is the
  ticket that would stop compat regressions going unnoticed, and it is a good unattended task
  because it is verifiable without a human eye.
- **CLD-015 cloud anti-repetition** — has its own agent prompt in that folder, and the
  coastline foam work this session is a close analogue (fixed-frequency noise with no distance
  fade). Same class of bug, same class of fix.
- **AST-019 clutter/CfgSurfaces semantics** — grass and clutter work, but the owner has open
  questions about whether grass placement honours the map's own surface definitions. That is
  precisely this ticket.

**What I did not do:** I did not read the 253 KB roadmap end to end, and I did not check each
ticket against its own acceptance criteria. I checked the ledger's status field, the decision
records, and the code for the formats I could verify by loading a world. Treat the three
"demonstrably working" entries as evidence to re-status those tickets, not as proof they meet
every criterion the roadmap states.
