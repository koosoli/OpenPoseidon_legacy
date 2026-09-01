# AUD-001 — Engine audit against "clean, modern, six-generation compatible", and the roadmap revision it implies

**Date:** 2026-09-01
**Branch:** `asset-obs-001` @ `8f948475` (287 ahead of `consolidated-v2`, 0 behind)
**Scope:** read-only survey of code and decision records; no binary was run, no source was changed.
**Goal audited against (owner's words):** "a clean, modern engine that is compatible with OFP, Arma 1, Arma 2,
Arma 3, DayZ and Reforger maps and assets."
**Companion edit:** `POST-DLSS-MODERNISATION-ROADMAP.md` (same commit) — the confident changes are applied
there; the contentious ones stay in §B here as proposals.

Every line below carries a citation or sits in §A5 marked unverified. Paths are repo-relative.

## A1 — Legacy patterns that fight the modernisation

### Wall clocks that still reach decisions

SIM-813's claim ("the last wall-clock budget in the simulation") holds for the AI stage — `rg
'CompareSectionTime|StartSectionTime' engine/` finds only the `PoTime` declarations and comments
(`engine/Poseidon/AI/AICenterImplPreview.cpp:484-499` keeps the old code as a comment beside the counted
`GroupScanBudget`). It does **not** hold for the frame as a whole:

| site | what the clock decides | class |
|---|---|---|
| `engine/Poseidon/World/Terrain/LandSave.cpp:3510,3581` | `steady_clock` admission budget: how many objects the stream admits this tick is a function of CPU speed (`:3495-3540` say so openly: floor-bound early, cap-bound late) | ration, decision by side effect |
| `engine/Poseidon/World/Terrain/LandSave.cpp:3219` + `Landscape.hpp:699` | `steady_clock` dt, camera velocity, prefetch lead: which residency window is admitted/evicted | decision |
| `engine/Poseidon/World/Scene/SceneDraw.cpp:1094` via `Graphics/Core/Engine.cpp:326` | `GetAvgFrameDuration(4)` (wall) writes `_lodInvWidth` (`:1060`): LOD is chosen by measured frame time; `render-snapshot-design.md:196` already flags this mutator | decision |
| `engine/Poseidon/World/WorldInit.cpp:154-156` | `GRandGen.SetSeed(GlobalTickCount() + seconds)` unless `POSEIDON_RNG_SEED` (`engine/Random/randomGen.cpp:230`) | decision (seed) |
| `engine/Poseidon/Core/Game/GameLoop.cpp:292-309` | `GlobalTickCount` to `deltaT` to number of fixed steps | expected; the fixed step is the design |

**Reconciliation with ARCH-001 (same day).** ARCH-001 measured that the admission budget does
not reach an *AI* decision on `perf_combat` (streaming budget 128x apart, AI behaviour oracle
EQUIVALENT over 902 ticks) and noted a floor guarantees every object is eventually admitted.
Both statements hold: the clock decides the *rate* and therefore *which* objects are resident on
a given tick, which is machine-dependent state visible to rendering, collision and replay even
when the AI oracle cannot see it on that scene. The replay-blocker framing here is about the
resident set, not about AI perception; ARCH-001's experiment does not retire it.

Diagnostic-only (checked, no decision reached): `SimStageGraph.cpp:229-232` (header `:205-215` forbids
decisions), `AICostAccum.cpp:18`, `Pass1Submit.hpp:138`, `SceneDraw.cpp:1791`, `LandscapeRender.cpp:1590`,
`PhysicsWorld.cpp:84-87` (steps with sim delta), `RainVolume.cpp:647-673` (integrates `Glob.time`),
`SmokeVolume.cpp:1000-1516`, `Scripts.cpp:439-483` (inside `#if DIAG_DEBUG >= 10`; the real script ration is
`maxExecute`, `:441,470`), every `GetTickCount` in `WorldInit.cpp:480-989` / `WorldImpl.cpp:1376-2175`
(load-phase logging). `Glob.clock` is the in-world clock, not a wall clock.

**Consequence for Phase 13 (record/replay):** the two `LandSave.cpp` clocks make the resident object set
machine-dependent, so a replay of a moving camera is not reproducible across machines even with
`POSEIDON_RNG_SEED` and lockstep. REN-THR-002 could not find why two runs of one scene diverge after ~24
frames; these are candidates it did not list.

### Raw allocation and per-frame string work on hot paths

Near-absent, which is a real result. `AIGroupImpl.cpp:858` (`new Target` in `AddTarget`) is guarded by a
linear `_targetList` scan `:845-853` that runs on every call — the scan, not the `new`, is per-tick cost.
Radio messages `AIGroup.cpp:1523`, `AIUnitImpl.cpp:1980,1986` are event-conditional. The one PERF-015 sibling
that survives: `engine/WgpuRenderer/EngineWgpu.cpp:4262` builds a `std::string loadName` **per section per
frame** in `DrawSectionTL`'s water/procedural branch; PERF-016 cached the name into
`MaterialClass::baseLoadName` (`:333-334`) but this branch still constructs. `:162-181`
`ContainsNoCaseAllocating` is kept only as the `WGR_SECTION_CLASS_CACHE=0` A/B arm.

### Global mutable state against Phase 15

| global | uses / files | planned in `ownership-authority.md` / `render-snapshot-design.md`? |
|---|---|---|
| `GEngine` (`Graphics/Core/Engine.cpp:22`, alias `GLOB_ENGINE` `Engine.hpp:2779`) | 1445 / 111 | **silent** — appears only as a call target (`render-snapshot-design.md:68`); a render-thread split moves exactly this object and no row owns it |
| `GWorld` (`World/WorldSetup.cpp:160`) | 1412 / 148 | row 1.5; S4 |
| `Glob` (`Core/GameState.cpp:445`) | 1212 / 172 | rows 1.1–1.4 |
| `GScene` (`Scene.cpp:66`) | 478 / 93 | S3 (`_lights` by value) |
| `GLandscape` (`Landscape.cpp:1124`) | 413 / 99 | §2 pull set |
| `GSoundsys` 156/22, `GStats` 100/24, `GNetworkManager` 70/18, `GDebugger` 44/15 | | silent |

Draw-context globals a render thread would inherit: `ShapeDraw.cpp:224-228` (`GCurrentConformPlane`,
`GCurrentIsVegetation`, `GCurrentFoliageKind`, `GGpuTerrainConform`), `PresentationSnapshot.cpp:54-55,272`
(`GGeneration`, `GResourceEpoch`, `GSnapshotFresh`), `EngineWgpu.cpp:1887,6220-6223` (five static counters),
`TextureWgpu.cpp:792` (`thread_local GTopMipHandoff`), `ClipShape.cpp:39-40` / `TransLight.cpp:43-50`
(`StaticStorage` scratch, single-threaded by construction — a Phase 8 worker touching draw code hits these).

## A2 — Dead or inert code

- `#define RptF(...) ((void)0)` (`Foundation/Framework/DebugLog.hpp:61`): **284** call sites compile to
  nothing (PARKED.md says 274; ten more since it was written).
- `BACKGROUND_AI` is `0` at `WorldSetup.cpp:163` and `WorldInit.cpp:92`, gating `#if` blocks at
  `WorldSetup.cpp:1182,1190` on a `SecondaryThread` class declared and never defined (PARKED.md).
- **Undocumented disabled code, 9 of 10 sites** (only `_ENABLE_CHEATS`, `PoseidonPCH.hpp:44`, is mentioned
  anywhere in docs): `#if 0` at `Scene/Thing.cpp:766-782` and `Network/NetworkMisc.cpp:712-735`;
  `_ENABLE_PERFLOG 0` `PoseidonPCH.hpp:26`; `ENABLE_ACM_CODECS 0` `Audio/Core/Format/WaveFile.cpp:16`;
  `ENABLE_OBJECT_INDEX 0` `LandSave.cpp:1234`; `ENABLE_STATS 0` `Simulation/Simul.cpp:536`;
  `ENABLE_OVERLAPPED_IO 0` `IO/FileServer.cpp:23`; `FOREST_PROXY_ENABLE 0` `Scene/ObjectClasses.hpp:168`;
  `_ENABLE_RANDOM_INTEGRITY_CHECK 0` `Network/NetworkServer.cpp:121`. None in `engine/WgpuRenderer`.
- **Env-var levers: 349 distinct names read in code, 191 in docs, 185 read but documented nowhere**
  (40 `POSEIDON_*`, 145 `WGR_*`; largest clusters `WGR_MOON*` ×11, `WGR_TERRAIN_*` ×9, `WGR_ROAD_*` ×7,
  `WGR_DLSS_*` ×7). Seven are **documented and read by nothing**: `WGR_OBJECT_STREAM_BUDGET_MS`
  (AST-001:68), `WGR_OBJECT_STREAM_REGISTER_MS` (HANDOVER-20260817:88), `WGR_FORCE_OPAQUE_TEX`
  (MAT-052:65,120), `POSEIDON_NO_HUD` (REN-THR-004:358), `WGR_TERRAIN_LOD` (WLD-023:79), `WGR_MATERIAL_VIEW`
  (handover 20260812b:192), `WGR_ENV_GATES_ARE_INERT` (a capability id, not a var). The README's rule
  "check the installed binary actually has a lever before you A/B it" (`scripts/README-farfield-bench.md:167`)
  exists because this class of error has already happened; the seven are its standing inventory.
- **Dev-panel bindings with no reader outside `Dev/Debug/DebugOverlay.cpp`: 19 of 60** — `thirdPerson`
  :1242, `s_zeus{Distance,Heading,Count}` :1347-1350, `todHours` :1797, `gPosPrecision`/`gPosCompactHud`
  :2228-2229, `s_vd` :2303, `showLoc` :2327, `s_console` :2457, `selectedEnabled` :3173, `forcedLayer` :3572,
  `autoTod` :4870, `sunDeg` :5028, `forcePhase` :5142, `forcedK` :5152, `rayDensity` :5378, `flareOn` :5413,
  `enableFrustum` :5515. Some are consumed a few lines later in the same file; the ones that are not are
  DZ-007's dormant checkbox again. Not verified one by one here.
- `WatchDog` (Phase 14 stall detector) is **no longer inert** — `FileServer.cpp`, `LandSave.cpp`,
  `ObjectStreamPrepare.cpp` call it (`9be22c81`). ARCH-001's "called by no production code" is stale.
- FSR 1 is shipped (`engine/WgpuRenderer/rust/src/fsr1.wgsl`, `upscaler.rs:244-368`, commit `74d891d6`);
  the Phase 1 table still listed it as an optional experiment. Fixed in the companion edit.

## A3 — Per-generation compatibility, as documented

Sources: `compat-c1-capability-matrix.md` (generated 2026-08-31 from `status-ledger.yaml`; **no DZ-*, RFG-*,
ARF-*, AST-016*, MAT-054/055, WLD-023..025 row exists — DayZ appears only as a passing count, Reforger
nowhere**), the decision records named per cell, `tests/unit/.../Compat/test_compat_smoke.cpp`.

| gen | worlds | models | textures | configs | animations | materials |
|---|---|---|---|---|---|---|
| OFP | loads (OPRW 2/3, `LandSave.cpp:1640`) | loads (ODOL 7 + MLOD; 1,661 routed, 0 refused) | loads (3,283-texture TAGG census) | loads | loads (739/739 RTM) | n/a (CfgMaterials) |
| A1 | loads (OPRW 18/20, WLD-022) | partial (ODOL 40; 18,697/18,697 LOD bodies close) | **unmeasured** | **broken**: `config debin` fails on A1 `config.bin` (AST-016A:153-158) | partial (1,379 RTM read; `vertexWeights` never measured, AST-016-017:70) | partial (4,859-RVMAT witness; terrain family NOT_PRODUCED) |
| A2/OA | loads (OPRW 24 census) | partial (534/534 Takistan) | **unmeasured** (one `_can.paa`) | **unmeasured** (same debin failure, no ticket) | **broken**: 8 of 3,493 RTM (0.2%), BMTR v3 unread | partial (TerrainX 3,531/3,531; Multi 4-layer NOT_PRODUCED); OA foliage shard cause NOT_PRODUCED, n=2 |
| A3 | loads (OPRW 25, byte-exact ×3) | loads (ODOL 73: 1,313 models; rev 75 refused by design) | partial (channel semantics n=181/40/47; SWIZ not applied) | **broken**: `A3_CONFIG_PARSING` UNSUPPORTED | **broken**: 39 of 6,157 RTM (0.6%) | partial (8 of 23 shader families schema'd) |
| DayZ | loads (OPRW 29 both worlds; Chernarus+ 24–29 fps, WLD-025) | loads (ODOL 54 ×8,203) | census only (DXT1 ×18,249, DXT5 ×9,947) | **unmeasured** | **broken by construction**: 0 `.rtm`, 8,484 `.anm` unread | partial (31,045 RVMAT counted; water DZ-002..008; runtime binding never censused) |
| Reforger | partial by conversion (`.pak` to OPRW 25; native `.ent` parses, not wired, RFG-002) | partial (`.xob` offline export; `.et` reader missing) | loads (`.edds` runtime-wired) | n/a | **absent** (no format identified) | **broken**: `MaskMap`/`BCR` 0 of 1,793 deployed `.emat` (RFG-003); colour-only dropped (MAT-054, corpus not re-exported) |

Test reality: the cross-generation smoke (`test_compat_smoke.cpp`, three cases) **skips on CI by its own
header** (`:44-48`) and skips in this worktree (`packages/` here holds only `Demo`); it runs only from the
main checkout, where `packages/{a1,a2,a3,dayz}-compat` hold 98 / 4 / 1,947 / 2 files. Committed fixtures
(`tests/fixtures/`: p3d 27, paa 16, rtm 2, wrp 1) are all OFP-era or synthetic; zero `.edds/.emat/.xob/.pak`
fixtures (those tests build bytes in memory). **DayZ has zero dedicated unit tests and no matrix row.** The
Trident integration suite (565 files) mentions no non-OFP generation; CI runs 111 of 117 scenarios on GL33
and none on WGPU (REN-GL33-001). So the roadmap baseline's "compatibility layers for Arma 1, Arma 2, Arma
3, DayZ and Reforger content" is true of readers and false of any gate that would notice one breaking.

Two open playtest defects sit in no track: the bridged A1 T-72 turret vanishing when crewed (AST-016D,
nine theories dead) and the OFP cockpit transparency (HANDOVER-20260826:9).

Unit-test coverage by generation (TEST_CASEs whose name/tag names the generation): OFP 301, A1 27, A2/OA 9,
A3 26, DayZ **4**, Reforger 38 — and no generation has a decision record about its config format at all (a
first-lines grep for `config.bin|rapified|CfgVehicles` over the 150 records returns zero).

## A4 — Measurement blind spots (beyond today's four)

The CPU phase profiler:

1. **`setup` is marked twice per frame** (`World/World.cpp:913` and `:1650`) and `Mark()` is `+=`
   (`Dev/Diag/FrameProfiler.hpp:83-88`), so nothing enforces the "phases partition the frame" rule the
   README asserts (`scripts/README-farfield-bench.md:280`, `farfield-compare.py:174-176`). The second bracket
   is scripts, camera derivation, HUD and the water harness; the README's shape rule "`setup` dominant means
   input/network" (`:306`) points at the wrong half.
2. **`sim:step` is absent from `triPerfStats`** (`Game/Commands/GameStateExtTestRender.cpp:423-438` prints
   `setup`, the draw aggregate, `hud..swap`; `PhaseSimStep` is in none of the ranges), so the integration
   suite's only machine-readable phase string cannot see the engine's largest CPU phase.
3. **`land:gnd` / `land:obj` read 0.000, not absent**, whenever `Landscape::Draw` is skipped
   (`World.cpp:1736`, `:1894-1916`); their time folds into `drw:land`. `farfield-compare.py` understands
   only `-1` as "did not run" (README `:243`) and a CPU phase can never emit it. `land:gnd` also swallows
   the whole `DrawRect` prologue (`Terrain/LandscapeRender.cpp:1631-1688`) and object streaming (README
   `:288`) — a phase named terrain whose 40-60% on heavy worlds is admission.
4. **`swap` is `NextFrame()` alone** (`World.cpp:2045-2050`): present, GPU wait and `FlushPendingScreenshot`
   — hence PERF-018's shutter. `frame_max` is still written (`apps/cwr/Game/GameApplication.cpp:278`) with
   no flag that the ring contains the capture frame, and `farfield-compare.py:469` prints it.
5. **Work after the last mark is in no phase**: `StreamingFrameRoll()` and the memory-footprint block run
   after `EndFrame` (`World.cpp:2050-2084`), and `BeginFrame` sits inside `Simulate` (`:178`), so any per-
   frame work in the app loop outside `Simulate` is outside `totalMs` entirely.
6. **No 1% / 0.1% low anywhere.** `FrameProfiler.hpp:74` is a 256-frame ring exposing avg/p95/max
   (`:70-71`); PERF-016 names low percentiles the right metric. `AvgFps()` (`:124-128`) is over whatever is
   in the ring; `Reset()` (`:140`) is called only by `triPerfReset`, never by the bench, so the window is
   "the last 256 `Simulate` calls before the shutter, shutter included".
7. **Two clocks in one number**: the panel's "Simulation ms" (`Dev/Debug/DebugOverlay.cpp:4441`) subtracts a
   one-second window (`EngineWgpu.cpp:2317-2323`) from a 256-frame ring mean and clamps to 0 (`:4442`)
   instead of reporting the mismatch. "Render thread %" is drawn when `rt.available`, not `rt.enabled`
   (`:4429`; `EngineWgpu.cpp:3088-3094` accumulates unconditionally), so it is labelled and shown on the
   inline path.

Counters where 0 means "did not run":

8. **The whole `streaming.texture` block is behind `WGR_TEXTURE_STREAM_STATS`** (`TextureWgpu.cpp:819-829`)
   — counts included (`uploads` `:1593`, `fallbacks` `:1562`, `preparedHits` `:1452`, `blockBytes` `:1449`)
   — and the JSON (`GameApplication.cpp:313-321`) has no `valid` sibling, unlike `objects.valid` (`:207`),
   `preparer.valid` (`:344`), `gpu_residency.valid` (`:393`). `frameUploads` is unconditional (`:1590`), so
   `frame_peak_uploads` can be nonzero while `uploads` is 0 in the same file. `streaming.fileserver` and
   `prepared_store` (`:326-342`) have the same gap. The comment at `TextureWgpu.cpp:1440-1443` argues a
   diagnostics opt-out must not disarm the byte accumulator, then leaves it behind the same flag.
9. **`lights` is always emitted and always 0-defaulted** (`GameApplication.cpp:296-302`,
   `Dev/Diag/LightingDiag.hpp:19-21`); its only writer is the wgpu path (`EngineWgpu.cpp:2695-2705`), and
   `cap = 0` is legal (`WGR_MAX_ACTIVE_LIGHTS=0`, `:2681`). Track B.1's "measured light-cost gate" rests on an
   instrument that cannot say "did not run".
10. **`streaming.snapshot.consumed_age = 0`** is both "in sync" and the initial value
    (`Dev/Diag/SnapshotDiag.hpp:44-48`); `firstDropGeneration = 0` means "never dropped" and collides with
    a real generation (`:42`).
11. **GPU regions can carry stale values for every region at once**: `gpu_timers.rs:389-391` returns before
    resolving when nothing was written, `:392-398` drops the sample when the ring is saturated, `:463-466`
    keeps last values on map failure; `timings()` (`:474-482`) carries no frame stamp. Roadmap 3.1's "fixed
    and re-verified" covers the per-region `written` mask (tests at `:507`, `:544`), not these three paths.
    Separately, a region opened N times per frame reports the last occurrence (`:342`; B.4 documents it), so
    "Grass shadow" is one cascade and the panel's "% of GPU frame" (`DebugOverlay.cpp:4235,4378`) inherits it.

Ratios and harness:

12. `farfield-compare.py:484-492` divides the *cheapest repeat's* GPU total by that repeat's *ring-average*
    CPU frame and calls it "GPU share"; the caveat is attached to the row above (`:494-497`).
    `EngineWgpu.cpp:8661` still prints `(frame - outerSum)/frame`, which goes negative when the containment
    table is incomplete — PERF-010's failure remains expressible. `StreamingTab.cpp:195-198` clamps the
    "drained fully" row with `std::min`, reporting 0 exactly when the four stop counters over-sum, which is
    the condition the row exists to catch.
13. **Three harnesses, three resolutions, none the play resolution**: world mode forces 800×600
    (`scripts/farfield-bench.ps1:307-314`; `GameApplication.cpp:135-139` says it overstates by ~25%),
    `farfield-movecam.ps1:156-157` 1600×900, `cockpit-capture.ps1:127-128` 1280×720; `-Width` is silently
    ignored without `-Windowed` (`farfield-bench.ps1:138-142`).
14. **World-mode warmup is a 25 s wall clock with no frame floor** (`farfield-bench.ps1:291-294`); at the
    README's own 9 fps more than half the 256-frame ring predates it and `sampled_frames` still passes the
    gate (`:547-552`), which is skipped entirely on older captures. `farfield-movecam.ps1:162` defaults to
    **one** repeat.
15. **Nothing verifies the exe and DLL are one commit.** `farfield-bench.ps1:262-267` checks the DLL exists;
    `Deploy.ps1:74-81,96-97,204-205` stamps `git rev-parse HEAD` after `Test-Path`, without checking `dist/`
    was built from HEAD (the silent staging failure, README `:73-79`) or that the tree is clean. Rule 0's
    "matching file times" is the only guard and the README itself says mtimes cannot settle it (`:245-254`).
16. The README still names the deleted `ai+veh` phase (`README-farfield-bench.md:279-280,307`).
17. The mip-feedback 87% (commit `3fa5dc97`) is a *scene* fact on `perf_combat`, where the 5.6 GB budget
    never fires (42 MB tracked); the commit says so. Not yet evidence of a residency defect.

## A5 — Unverified at time of writing

- The 19 dev-panel bindings in A2 were counted, not read; which are truly dormant is a half-hour job.
- The "sky cloud march 11.7%" figure is ARCH-001:117 (`perf_combat`, 11.26 ms GPU frame, 1.314 ms). It has
  a quality tier (HANDOVER-20260824:144, steps 128) but no roadmap line and no A/B since the SH fix.
- A4 items 1-16 are one survey's readings of the cited lines; none was exercised by running a capture, and
  the three GPU-timer stale paths (item 11) have no test that would show them.

## B — Roadmap revision

Applied in the companion edit (confident):

1. **Status corrections.** FSR 1 shipped; upscaler boundary "substantially done" per REN-UPS-001 §7 with
   items 1-3 of its §6 carried; the Phase 1 table carries the measured verdicts for streaming counters,
   dedup and the upload budget in the table itself.
2. **Simulation track: closed as planned, reopened as measured.** 10S exists (SIM-811 serial graph), 11S is
   done (SIM-801/803), 12S answered no, 13S is *already the ration* (PERF-017). What remains is Phase 8's
   command/commit stage and Phase 13's replay, and the first blocker of both is A1's machine-dependent
   admission set.
3. **A six-generation compatibility track (D) with per-generation gates**, replacing one baseline sentence
   and one continuous-foundation bullet. Gate = the smoke test running somewhere it can fail, one committed
   synthetic fixture per container format per generation, and a matrix row per generation. The
   "unmeasured" cells in A3 are the first tasks.
4. **Next three measured targets**, each with its number and its control:
   - `unitThink` 0.95–2.06 ms/tick (PERF-019): 6–12% of a 16.7 ms tick; gate `POSEIDON_AI_TIMELINE` diff.
   - Chernarus+ steady-state `land:obj` after PERF-016 (2.65 ms) **re-measured on Chernarus+, not Everon**,
     with the admission wall clocks (A1) replaced by counted budgets so the number is the same on two
     machines.
   - GPU: the sky cloud march, 1.31 ms / 11.7% (`perf_combat`), ahead of any grass reopening — grass is
     closed by PARKED.md's own rule, and the sky has a lever (step count) with no measurement since the SH
     fix.
   Mip feedback stays a *measurement* task until it is run on a budget-constrained world (Chernarus+ at the
   20,000-item cap), because on `perf_combat` the budget never fires.
5. **Phase 3.1 gains four rules:** `frame_max` is never quoted; 1% and 0.1% lows are added to the profiler
   before the next perf decision; a region opened more than once per frame reports its multiplicity; every
   sidecar counter block carries a `valid` sibling (A4 items 8-10).

Proposed here, not applied (owner decision, or needs a measurement first):

- **Rename `land:gnd`** or split streaming admission into its own phase. Cheap, but it re-baselines every
  stored capture and `farfield-compare.py`'s tables.
- **Make the admission budget counted, not timed** (A1). It changes what the world contains per tick on
  every machine; the preload mode shows the fill converges anyway (WLD-025), so the risk is pop-in on slow
  machines — the case the wall-clock ceiling was written to hide.
- **Assign `GEngine` an owner** in `ownership-authority.md` before Phase 5 step 6: 36 rows, and the
  most-used global in the engine is not one of them.
- **Delete the seven documented-but-unread env vars from their documents**, or land them; a lever in a
  decision record that the binary ignores will be A/B'd again.
- **Move the two open playtest defects** (turret, cockpit) onto track D rather than leaving them in
  handover files.
- **Give PARKED.md a pointer to A2's undocumented-disabled-code inventory** rather than a copy, so the
  list has one home; not done here to keep this commit to two files.
- **Retire the Phase 0 ship-gate checklist** into a status line: DLSS 4 is on trunk and hardware-gated,
  the matrix ran, and the checklist has no ticks and no one will tick it now.
