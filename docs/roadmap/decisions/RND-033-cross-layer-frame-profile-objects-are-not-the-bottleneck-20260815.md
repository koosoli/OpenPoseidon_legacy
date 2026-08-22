# RND-033 — Cross-layer frame profile: objects are not the bottleneck on any compatibility layer

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** PARTIAL — profile measured; one win implemented and A/B'd; several findings need re-verification on a clean binary.
**Related:** [RND-032](RND-032-single-lod-xob-export-cost-6-5x-of-gpu-frame-20260815.md)

## Why this exists

RND-032 closed a 6.5x object-geometry win on imported Reforger content. The obvious next question was
whether the same win was available on the Arma 3, Arma 2/OA and DayZ layers. **It is not, because
objects were never the bottleneck there** — and after RND-032 they are not the bottleneck on Everon
either. This record states where the time actually goes on each layer, so nobody re-runs the object
optimisation reflex a third time.

## Where the frame goes, per layer

All at 800x600, `WGR_OBJECT_STREAM_MAX_OBJECTS=200000` unless noted.

| world | format | GPU frame total | objects | dominant cost |
|---|---|---|---|---|
| Everon | Reforger XOB | 11.85 ms | ~0.9 ms | untimed passes |
| Takistan (Rasman) | Arma 2/OA ODOL | 12.04 ms | ~1.2 ms (10%) | untimed passes |
| Stratis (Agia Marina) | Arma 3 ODOL | **27.484 ms** | 2.6 ms (9.4%) | **water 28%** |
| Chernarus (Chernogorsk) | DayZ ODOL 54 | 20.15 ms (20k resident) | **1.9 ms** | **VRAM oversubscription** |

Takistan at a realistic budget is **at parity with Everon**, and carries 344,704 main-view triangles —
*fewer* than Everon's 575,807. Objects are ~10% of frame everywhere.

The native ladders are healthy, and the evidence for that is the **runtime LOD histograms**, which are
capture artifacts:

| world | histogram |
|---|---|
| Stratis | `[8, 358, 800, 1976, 541, 372, 0, 0]` |
| Takistan | `[70, 326, 1873, 598, 415, 225, 2, 0]` |
| Everon (after RND-032) | `[1, 0, 988, 2693, 13418, 1244, 2615, 300]` |

Placement totals, parsed from `PoseidonTools terrain objects`: **163,879** (Stratis) and **438,749**
(Takistan).

> **Retracted 2026-08-15.** An earlier revision of this record claimed a per-model corpus census —
> "340 Stratis models / 513 Takistan, 4.14 and 5.20 graphical LODs per placement, on the same 1..N
> resolution scale the `.xob` path writes". **That census was never run and those numbers are not
> measurements.** They were written up as though a subagent had reported them when it had returned
> nothing. The conclusion above does not depend on them — the histograms alone carry it — but the
> per-model LOD counts and the claim that native ODOL resolutions share the `.xob` scale are **open
> items**, not findings.
>
> What *is* measured about native ladders is narrower and lives in RND-032: `model inspect` on two
> specific Arma 3 models (`t_pinuss2s_b_f`, `cargo_house_v1_f`), each carrying exactly 4 visual LODs
> at resolutions 1,2,3,4. Two models are not a corpus.

**Stratis is the one layer that is genuinely slow: 2.3x the reference.** Obtained at full residency
163,953/163,953 with **zero contended samples** (48 gpuwatch samples: 41 sole process, 7 none) — the
cleanest run of the session, so this figure needs no steady-floor caveat. Registered 75,274; main
4,055 instances / 929,831 triangles; shadow 625,817; LOD histogram `[8,358,800,1976,541,372,0,0]`,
healthy. Ranked:

1. untimed passes ~12.4 ms (43-45%)
2. **water ~7.6 ms (28%)** — draw 2.972, planar reflection 2.976 (clouds 1.494 + terrain 0.882 +
   objects 0.512 + sky 0.088), FFT/foam sim 1.669
3. 3D colour sub-pass 3.699 ms (13.5%) — terrain colour 2.156 (**3x Takistan's**), objects 0.996,
   grass 0.189
4. objects ~2.6 ms (9.4%)

Water is the entire reason Stratis costs more than Takistan: it is a coastal view with ocean on
screen. That is inherent to the scene rather than a defect, and is tunable through the existing
`WGR_PLANAR` policy. Caveat: this run used the rebuilt DLL (the water gate plus other agents'
uncommitted Rust edits) while Takistan's 12.044 ms used the older DLL; the water gate is a provable
no-op here because water is visible, so every FFT/foam region reports positive in this JSON.

## The largest opportunity is unmeasured, not unoptimised

**~7.0 ms — 51-58% of every steady frame, on every world — is in passes with no GPU timer region at
all.** `timestamp_writes: None` on main-view sky, the volumetric cloud march and composite, bloom,
godrays, tonemap and UI. The *magnitude* is measured (as the `Frame accounting` residual, reproduced
on every steady frame of every world); the *composition* is inferred from which passes lack regions.

This is the top-ranked next action and it is an instrumentation task before it is an optimisation
task. Do not attack it by guessing which pass is expensive.

## Implemented win: skip water simulation when no water is selected

`engine/WgpuRenderer/rust/src/water/mod.rs` (+31/-1). The FFT / ripple / foam compute ran every frame
regardless of whether the CDLOD water selection was empty. It is now gated, with
`WGR_WATER_SIM_ALWAYS=1` restoring the old behaviour so **both A/B arms come from one binary**.

Same pose, same budget, full residency 200,000:

| | cheapest steady frame | attributed | water-sim regions |
|---|---|---|---|
| A `WGR_WATER_SIM_ALWAYS=1` | 7.603 ms | 3.887 | spectrum 1.247, FFT-h 1.382, FFT-v 1.382, compose 2.061, foam 1.425 |
| B default (skip) | **6.032 ms** | 2.659 | all **-1** (never ran) |

**-1.571 ms, -20.7%.** The `-1` values prove the gate fired; the attributed delta (-1.228 ms) matches
the removed regions; the two PNGs are visually identical. Water is Stratis's dominant cost
(draw 5.366 + planar reflection 3.179 = 42% of frame at the airfield pose), so this helps A3 most.

## DayZ: the bottleneck is VRAM, not geometry

- World loads **completely**: 2,971,218 of 2,971,218 placements retained, 2,352/2,352 models
  referenced, **zero** refused creates in every residency update. The default 20,000 budget is what
  limits the drawn world to a ~550 m radius — Chernarus averages 61 placements per occupied 60 m cell.
- Buildings **are** correctly textured: 39 of 5,390 baked sections untextured (0.72%), 0.057% of
  triangles. Offenders are container interiors, a mirror and penetration materials.
- LOD histogram is healthy: `lod0=193 lod1=1673 lod2=1640 lod3=1367 lod4=372`.
- **VRAM oversubscription** is the cost: 4.23 GB tracked against a 2 GB budget on an 8 GB card.
  `TextureBankWgpu::TrimResidency` skips any texture with `ResidentPins() != 0` and `RetainGpuModel`
  pins every texture of every registered model, leaving 1-35 eviction candidates against 1,043
  distinct models legitimately in view. The signature is decisive: content-independent passes
  inflated with the rest (water FFT horizontal 0.22 -> 3.24 ms, spectrum evolve 0.12 -> 2.76 ms). An
  FFT cannot care how many buildings are resident; that is the driver paging over PCIe. Wall clock
  5.6 fps vs 22.3 fps for the native control.
- Whole-texture eviction cannot fix this because the pinned set is genuinely in use. It needs
  **mip-level residency** — dropping high mips for distant / coarse-LOD models. A cheap interim
  measure is capping mip levels for sections only ever drawn at lod >= 3.

## Two open DayZ defects, diagnosed but not applied

1. **37 distinct proxy models never load.** `GetShapeName` -> `GetDefaultName(baseName, "data3d\\",
   ".p3d")` at `engine/Poseidon/IO/ParamFileExt.cpp:168` prepends `data3d\` to any proxy name lacking
   a leading backslash. Proven not-missing-data:
   `dz\structures\industrial\cementworks\proxy\misc_pallet.p3d` exists in `structures_industrial.pbo`
   at exactly that path minus `data3d\`. Casualties include the Chernogorsk ferris wheel, church
   crosses and hotel/house interior proxies. Proposed fix: in `GetShapeName` only, skip the prefix
   when the name already contains a path separator. Not applied — shared engine code on every world's
   proxy path, and OFP content was not smoke-tested.
2. **86.6% of Chernarus placements render the wrong colour.** `dz\plants\*` is 2,572,372 of 2,971,218
   placements (2,222,451 of them trees) against `dz\structures\*` at 360,865 (12.1%). DayZ authors
   foliage albedo brown/orange and relies on the `TreeAdv` shader's `treeColorization[] =
   [0.12, 10, 10, 0]` plus a WorldPos-mapped Stage3 map to tint it to season green; CWR-CE has no
   `TreeAdv` colorization and binds the raw albedo, so mid/distant trees are saturated yellow. The
   first hypothesis — that the RVMAT `ambient[] = [1,1,0.3]` was being applied literally — was
   **tested with an env-gated lever and disproved**; the trees stayed yellow.

## Measurement hygiene — read before trusting any timing in this session

Concurrent agents sharing one binary and one `dist\` corrupted measurements in three distinct ways.
All three are real and all three produced at least one false finding:

1. **Process killing.** Every capture script cleans up by stopping PoseidonGame PIDs that did not
   exist at *its* launch, so any run started after another agent's snapshot looks new to it. Captures
   died mid-run repeatedly (8 times in one agent's session, 3 in another's).
2. **Binary contamination.** Agents rebuild into the shared `dist\`. A rebuild at 01:32 deployed
   three other agents' in-flight, unfinished renderer edits, and every capture after that ran against
   them.
3. **GPU sharing.** Two resident PoseidonGame processes inflate *every* timed region by a common
   factor (measured 27.9 ms vs 7.6 ms frame; cull 2.214 vs 0.449; colour 0.565 vs 0.210) — a
   clock/sharing effect, not extra work. Quote the cheapest steady frame, not the median.

**A false finding this produced, recorded so it is not rediscovered:** "near-field terrain renders
white/untextured" was reported independently by two agents as a pre-existing defect. It is a build
artifact of (2). Measured on the same scene: clean binary 0.0% pure-white pixels, mean RGB
(210, 204, 162); contaminated binary 65.3% pure-white, mean RGB (222, 222, 211). It also appears in
the native CWA control, which no content-specific defect would.

**Also retracted:** an early Stratis sample reading `lod0=2335 lod1=111` with the rest zero is **not
reproducible**; every steady sample shows a spread (`[8,358,800,1967,526,372,0,0]`).

## Not closed

- The water win needs re-verification on a clean binary. Both its arms shared the contaminated DLL,
  so the *comparison* holds but the absolute numbers do not.
- "Trees vanish from the main view at budget 200000 on Takistan but still cast shadows" was observed
  on the contaminated binary and must be re-checked before it is treated as real.
- No owner review of any of the above.
