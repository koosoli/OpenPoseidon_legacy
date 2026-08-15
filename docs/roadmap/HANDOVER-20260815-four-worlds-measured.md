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

# OWNER FEEDBACK AFTER THE SESSION — READ BEFORE TRUSTING ANY NUMBER ABOVE

The owner looked at the deployed build and reported the following. Some of it contradicts
measurements in this document. **Where they conflict, he is right and the measurement is
describing a state he never occupies.**

## 1. Reforger is ~4 fps for him, not the 40.5 fps measured here

This is the most important line in the document. Every Reforger figure above comes from a
FIXED freefly pose, settled to `pending=0`, captured by auto-screenshot. He plays by MOVING.
If moving re-triggers residency streaming continuously, he is permanently inside the
fill-in transient (which this document already measures at 200 ms+ frames, i.e. ~5 fps) and
never reaches the settled frame being quoted.

**Do not report another Reforger number until you have measured a MOVING camera.** The
harness only does static poses; that is a hole in the methodology, not a detail. If a moving
camera does sit in the transient permanently, then the streaming transient is not "the thing
you see at launch" — it is the whole experience, and it outranks everything else here.

## 2. REGRESSION — buildings no longer cast shadows, interiors are lit through walls

He has before/after screenshots: sunlight now falls on an interior hall floor and ignores the
walls entirely, where it used to be correctly shadowed. He calls it a big regression, and he
is almost certainly right that it is recent.

**I probably caused this, and here is the mechanism to check first.**
`EngineWgpu.cpp:1460` gates the whole cascade system on

    shadowActive = _smCascadesValid && !_shadowCasters.empty()

and `SceneShadowPass.cpp:508-512` deliberately SKIPS every `GpuDrawCoverage::Full` object when
filling `_shadowCasters`. I turned `WGR_TREEADV_RETAINED` **on by default** this session, which
moves far more objects onto the retained path and therefore to Full coverage — plausibly
emptying that list and disabling cascades wholesale.

**First test of the night, it is five minutes:** run a world with an interior and compare
`WGR_TREEADV_RETAINED=0` against the default. If shadows come back with it off, that is the
cause, and the fix is the one the shadow audit already described — drop `!_shadowCasters.empty()`
from the gate and gate on `_smCascadesValid && (!_shadowCasters.empty() || gpuDrivenShadowsActive)`,
**and** set `_smCamPos` from the current camera in `InitDraw`, because today it is written only
inside `AddShadowCaster` and would otherwise be a stale value from an earlier frame.

If that is confirmed and the proper fix is not quick, **turn the default back off**. A 2x
framerate win on one world does not pay for every building in every world losing its shadow.

## 2b. THE SAME FLAG BROKE PINE TRUNKS — and that is why it is now reverted

He also reports Arma 3 pine trees have lost their trunks again, "something we once fixed".
It was fixed, and the fix is at `EngineWgpu.cpp:2711-2727` on the **CPU per-draw path**: its
comment says that without the matching discard "the black transparent texels depth-write in
the opaque fallback and completely hide the pine's trunk behind its cards". The RETAINED path
has no equivalent, so moving a pine onto it loses the fix.

That is the third regression traceable to one flag I turned on, alongside the shadows above
and the near-tree LOD. **I have reverted `WGR_TREEADV_RETAINED` to default OFF** and rebuilt
and deployed. The measured prize was real (`land:obj` 42.702 -> 0.185 ms, Stratis 17.24 ->
44.69 fps) but it is one world's framerate against correct shadows, trunks and near detail on
every world.

**Do not turn it back on until the retained path can do three things**: carry the trunk
discard, be counted as a shadow caster (the `_shadowCasters` gate), and register a model's
finest LOD rather than skipping levels that have no GPU-owned section. Then re-check an
interior for shadows and a pine for its trunk before making it a default again.

This also means **Stratis's 44.69 fps is no longer what ships.** Treat the Stratis figures in
this document as "what is available once the retained path is correct", not as current.

## 3. Still broken, unchanged by this session's work

- **Arma 2 roads are still white AND still visible through mountains.** My depth-bias fix
  (ndc² scaling in `shader3d.wgsl`) did NOT fix the see-through — so either the bias is not
  the mechanism, or it is not the only one. The white is a separate, still-undiagnosed bug.
  **The owner's lead is the one to follow: roads are correct in Arma 1 and DayZ. Diff a
  working road against a broken one rather than reading the broken one alone.**
- **Trees near the camera still show a far LOD, and cast no shadow.** Note this persists after
  the governor default was reverted to 16, so the LOD half is NOT only the governor — see the
  registered-LOD filter note (a model whose finest GPU-registered LOD is coarse, because
  `RegisterGpuModel` skips levels with no GPU-owned section, is pinned there permanently).
- **Stones still transparent from one side** (the known partial-alpha granite case, MAT-047).
- **Many buildings untextured or drawn wrong** — consistent with the Reforger `.emat` finding
  above (zero normals and zero speculars bound anywhere).
- **Arma 3 light orbs persist.** IMPORTANT PRACTICAL NOTE FROM THE OWNER: **the orbs take a
  while to appear — the world must load before they show.** So a capture at his coordinates
  (freefly `2736.50, 5993.66, 45.19`, az 4.5, el −27.1) needs a long warmup, or you will
  photograph a clean frame and wrongly conclude it is fixed.

## 3b. Water foam — two more specific asks

The lattice is gone but he wants two further changes, and they are separate:

- **The foam reaches too far into deep water. Pull it closer to the coast.** That is the
  band width, `water.wgsl` around the `eff_depth / max(wp.foam_width * 2.2, 0.4)` term — the
  deep-side extent was already tightened 1.95 -> 1.55 this session and wants to go further.
  The land side carries a no-hard-line guarantee; tighten the DEEP side only.
- **It still tiles.** The 1.35 x 5.3 m rectangular lattice was fixed, so this is the residual
  from the remaining analytic-noise octaves. The LOD-aware filtering added this session fades
  each octave toward its mean as its cells approach pixel size; the handover for that work
  flags `base_gain` / `pattern_gain` and the `0.45` in `far_fade`, plus the
  `smoothstep(0.30, 0.85, ...)` cells-per-pixel handover point, as the knobs. A second
  decorrelating octave at an incommensurate frequency is the usual next step.

`WGR_WATER_SHORE_FOAM_LEGACY=1` still restores the original for comparison.

## 3c. Feature request — iris/eye adaptation when looking at the sun

His words: "when the player looks in the direction of the sun, the iris adjusts and it gets a
bit darker, because now the sky is often just almost white out when looking at the sun."

This is auto-exposure / eye adaptation, and it is a FEATURE, not a bug fix — do not conflate
it with the Arma 3 light-orb bug, which is a different thing (lights mis-read as suns). The
engine already has an HDR path (`effective gates: hdr=true`), so the question is whether it
has any adaptation term at all or is tone-mapping at a fixed exposure. Check that before
designing anything. A slow-adapting luminance feedback on the frame's average or a
sun-direction-weighted term are the usual approaches; the risk to avoid is adaptation that
pumps visibly as the camera turns, which is worse than a flat exposure.

## 4. A target to aim at, in his words

**Arma 3's lighting looks great. Arma 2's used to, and no longer does.** That is a concrete,
checkable goal: find what Arma 3 does in the lighting/material path that Arma 2 no longer
gets. It is likely the same class of finding as the road one — a working reference exists
in-tree, so diff the two rather than theorising about either.

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
