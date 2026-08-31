# SIM-815 — Three ways real time still reaches an identical simulation step, and the per-tick recorder that can tell them apart

**Date:** 2026-08-31
**Branch:** `agent-sim-divergence`, based on `safety/asset-obs-001` at `cc20b525`
**Roadmap:** Phase 8 — deterministic simulation.

**Related:** [REN-THR-005](REN-THR-005-the-divergence-was-the-loading-screen-and-then-the-step-count-20260831.md)
(the measured fact this explains), [REN-THR-003](REN-THR-003-what-in-the-published-frame-is-a-function-of-the-clock-20260831.md)
(the LOD-governor→smoke coupling, confirmed live here), [SIM-813](SIM-813-deterministic-ai-group-scan-20260831.md)
(the wall-clock AI budget, already removed from this build), [SIM-812](SIM-812-deterministic-command-commit-20260831.md)
(the mutation audit and the first clock sweep), [SIM-808](SIM-808-first-divergent-boundary-20260831.md)
(`StateTimeline` and the anti-vacuity rule).

## The fact to be explained

REN-THR-005, lockstep arm: three runs of `perf_combat`, aligned on the first
world frame, take the **same number of fixed steps in every one of 40 frames**
(40/40, verified) — and the published frames still differ in 28 of 40, and
**only in the position-dependent slices** (`cameras`, `draws3d`, `palette`,
`shadow_casters`, `verts`). `terrain_nodes`, `terrain_batches`,
`grass_batches`, the 2D batch/command structure and clear/fog **never**
differ. Identical step count, identical starting state, divergent positions.
SIM-813's wall-clock AI budget is already out of this build, so it is not
that.

This document is (1) the audit of what inside one fixed step can still be a
function of real time, (2) a per-tick recorder one level below the frame hash,
with ablation evidence, and (3) the run recipe that discriminates the
candidates, cheapest first.

---

## 1. The audit

### 1.1 The clock sweep, re-run with a denominator

`grep -rn "GetTickCount|timeGetTime|QueryPerformanceCounter|steady_clock|
system_clock|high_resolution_clock|GlobalTickCount"` over
`engine/Poseidon/AI`, `engine/Poseidon/World` and `engine/Evaluator`
(`.cpp/.hpp/.inc`): **76 hits, all classified**. This is a superset re-run of
SIM-812's sweep, not a citation of it.

| Class | Count | Feeds a decision? |
|---|---:|---|
| Load-time log timers (`WorldImpl.cpp` 21, `WorldInit.cpp` 12) | 33 | No — every one brackets a `LOG_DEBUG(Core, "LOAD: ...")` |
| Stat-only timers (`RainVolume` 6, `SmokeVolume` 4, `PhysicsWorld` 2, `BuildingInterior` 2, `SceneShadowPass` 2, `SceneDraw.cpp:1732` 1, `LandscapeRender.cpp:1590` 1) | 18 | No — each writes a `_lastXxxUs` stat or a perf-trace timestamp; none is read back into a branch |
| Cheat-build memory-footprint sampler (`World.cpp:1893/:1897`, `GlobalTickCount`) | 2 | No — gates a log line |
| MP-report filename (`AICenter.cpp:628-629`) | 2 | No |
| Comments referencing the budget SIM-813 removed (`AIGroupScanBudget.hpp:10`, `AICenterImplPreview.cpp:483`) | 2 | No — text, not code; the fix is confirmed present on this branch |
| **Object-streaming decisions** (`LandSave.cpp` 11, `ObjectStreamPrepare.cpp` 7, `Landscape.hpp:699` 1) | 19 | **Yes** — candidate C3 below: the admit budget/ceiling (`LandSave.cpp:3452/:3515/:3539/:3548`), the prefetch camera-velocity estimator (`LandSave.cpp:3182-3218`), the prepared-texture TTL and worker timings (`ObjectStreamPrepare.cpp`) |

**Zero clock reads remain in the AI, vehicle, physics or script per-tick
decision paths.** The step's own clock, `Glob.time`, advances by a constant
under lockstep (`World.cpp:1969` inside the Clock stage). `BACKGROUND_AI` is
`#if 0`-dead (`WorldInit.cpp:91`; undefined in `World.cpp`, so its `#if`
blocks compile out) — the AI stage runs synchronously inside the step.

But a clock **read** is not the only way real time gets in. Two inputs that
are not clock reads at all carry it into the step, and both are live on a
combat mission:

### 1.2 C1 — the shared RNG's draw count is a function of GPU frame time

The chain, every link verified in the compiled branch (`DENSITY_LOD` is
defined nowhere, so the `#if !DENSITY_LOD` implementations are the live ones):

1. `Scene::AdjustComplexity()` runs once per rendered frame
   (`SceneDraw.cpp:1834`) and reads `GEngine->GetAvgFrameDuration(4)`
   (`SceneDraw.cpp:1095`) — **real GPU/wall frame time, which lockstep does
   not and cannot pin** — to move `_lodInvWidth`
   (`SceneDraw.cpp:1176/:1191`).
2. `Scene::GetSmokeGeneralization()` returns `_lodInvWidth * 0.1f`
   (`SceneDraw.cpp:1264`).
3. Every particle source folds it into `_generalize`
   (`Smokes.cpp:100/:1073/:1139`) and paces its spawns with
   `_nextTime += _interval * _generalize * _generalize`
   (`Smokes.cpp:103/:1076/:1142`).
4. Each spawn takes **seven sequential draws** from the one shared `GRandGen`
   stream (`Smokes.cpp:106-112`, `:1080-1086`) — the stream everything else
   also draws from, in tick order: AI (`AIUnitImpl.cpp:1959`), radio pacing
   (`AIRadio.cpp:1607/:1611/:1622`), ballistics dispersion, soldier and
   vehicle sim (SIM-812 §1C.3 lists the sites).

So two runs whose frames took different GPU time spawn a different **number**
of cloudlets in some tick, consume a different number of draws, and from that
tick on every sequential consumer in both runs reads a shifted stream. On
`perf_combat` — vehicles with exhaust and dust, weapons with muzzle smoke —
this is live from the first ticks, not only after the first shot.

REN-THR-003 named this coupling statically and could not observe it on the
abel bench (no smoke). This audit confirms every link compiles and adds that
the *consumer set* of the shifted stream is the whole simulation.

**Why it fits every constraint:** survives lockstep (the input is frame
duration, not `deltaT`); moves positions (AI wander, dispersion, radio pause
lengths) and nothing structural; terrain and grass use the **positional** RNG
(`RandomValue(x,z)`, pure — `randomGen.hpp:31-38`) and cannot be reached by a
sequential-stream shift, which is exactly the observed "positions always,
terrain/grass never".

### 1.3 C2 — the radio drains on audio playback completion

`RadioChannel::Simulate` (`Audio/Speaker.cpp:88-125`) is called from inside
`World::PerformAI` (`WorldImpl.cpp:455-494`, per center and per group, every
tick). It **returns early while `_saying->IsTerminated()` is false** — i.e.
while the voice wave is still audibly playing — and only then counts down
`_pauseAfterMessage` and calls `NextMessage()` (`AIRadio.cpp:1576`).
`NextMessage` fires `Transmitted()` on the completed message — which issues
commands and can build whole subgroups (SIM-812 §1E) — and takes two
sequential RNG draws for the pause and the sentence variant
(`AIRadio.cpp:1611/:1622`).

Audio playback is decoded and clocked by the sound device in real time. Under
lockstep the tick at which a wave reports terminated is therefore **not** a
function of tick count: two runs hear the same contact report end on
different ticks, the follow-up order lands on different ticks, and both the
command timing and the RNG stream shift. On a combat mission the radio is
busy from the first contact.

(The path is gated by `GetAcceleratedTime() <= 1` — at accelerated time the
engine substitutes the message's nominal duration (`AIRadio.cpp:1633-1636`),
which is deterministic. That substitution is also the cheap discriminator —
see §3.)

### 1.4 C3 — streamed-object admission changes the collision world on a wall-clock budget

`Landscape::UpdateModernObjectResidency` runs once per rendered frame from
the world's draw path (`World.cpp:1519`), on worlds loaded through the OPRW
path (`LandSave.cpp:2328` sets `_modernObjectStreaming`; classic worlds load
their whole object set at world load — `Landscape::LoadObjects` has **zero**
callers, so nothing streams cells at runtime there). The admit loop is
bounded by `steady_clock` budgets and ceilings (`LandSave.cpp:3452`,
`:3515-3556`), by the frame's cumulative upload bytes (`:3483-3489`), and by
which worker-prepared IRs happen to be ready (`:3560-3736`, skip-not-wait).
Each admitted placement ends in `ObjectCreate` (`:3777/:3782`) →
`Landscape::AddObject` (`Landscape.cpp:2674`) — the landscape grid that
`ObjectCollision` and the AI read **within the simulation**
(SIM-812 §3: `Collisions.cpp` reads `_objects(x,z)` at nine or more sites).

So on a streamed world, a building or tree exists in run 1's collision grid
at tick N and in run 2's at tick N+k, where k is a function of disk, worker
scheduling and GPU upload pressure. A vehicle that clips the tree in one run
and open ground in the other diverges permanently. An entity whose shape is
not yet admitted is not "an entity waiting on a load" — **the object does not
exist at all**: nothing in the sim branches on residency (no
`IsLoaded`-style guard anywhere in `Simulation/Collisions.cpp`), absence is
silent. Mission entities are unaffected — every `Shapes.New` in
`World/Entities` (13 sites) is a synchronous load at creation.

Whether `perf_combat`'s world streams is a fact about the mission, not the
engine; the recorder's `shapes=` column answers it from the first log.

### 1.5 The classes that came up empty

- **Address- or hash-ordered iteration feeding a decision:** all 29
  `unordered_map/set` uses in `AI/` and `World/` classified. Every
  pointer-keyed one is a lookup cache or a logging dedupe set
  (`Ballistics.cpp:328`, `ShapeAdapter.cpp:119`, `TransportCore.cpp:1425`,
  `SceneShadowPass.cpp:64` — render side); none is *iterated* to feed a sim
  decision. The sim's own orderings are array-ordered (SIM-803's territory).
- **Threaded completions other than streaming and audio:** the FileServer's
  completions reach the sim only through those two paths; the object-stream
  preparer workers contain no RNG (audited in `randomGen.hpp:44-48`) and
  their readiness only matters where C3 already looks.
- **`DistributeImportances`** moves entities between simulation buckets from
  camera distance (SIM-812 §2) — a pure function of state, so it is a
  *propagator* of an existing divergence, not a source. The recorder folds
  list order, so a bucket flip is visible as a `pos` divergence with an
  unchanged `ents` count.
- **Uninitialised or padding bytes reaching a decision: not audited.** No
  sweep exists that would make a denominator honest here; ASan/MSan on the
  mission is the tool, and this session did not run the game. Named as the
  one class left open.

### 1.6 Ranking

| Rank | Candidate | Why this rank |
|---|---|---|
| 1 | C1 — RNG draw count via LOD governor → smoke cadence | Live from tick ~0 on this mission, needs no event to trigger, and REN-THR-005's camera parting at world+2 wants a cause that is active immediately |
| 2 | C2 — radio drains on audio completion | Live from the first radio message; moves both command timing and the RNG stream; but needs a message in flight, so it cannot explain a divergence before the first transmission |
| 3 | C3 — streamed-object admission | The heaviest mechanism, but conditional on the world streaming, and its first effect needs an entity to touch a differing cell |

All three survive lockstep because lockstep pins `deltaT` and the step count
— not GPU time (C1), not the audio device (C2), not the disk (C3).

---

## 2. The instrument

### What was built

`engine/Poseidon/Core/TickStateHash.{hpp,cpp}` — std-only, ~200 lines: FNV-1a
folds with `HashPublishedFrame`'s discipline (length-prefixed items,
count-suffixed classes, bits-never-values), a fixed field registry, and a
running distinct-hash set. Unit-testable without an engine.

The hook — `World::RecordTickStateHash` (`World.cpp`, called at the end of
`StepSimulation`) — is gated on `POSEIDON_TICK_HASH=1`, resolved once; when
the variable is unset the entire feature is one static-bool branch per tick.
When on, each fixed step logs one line:

```
TickHash tick=<n> tms=<Glob.time ms> pos=<h> vel=<h> cam=<h> rng=<draws> shapes=<n> ents=<n> clouds=<n> all=<h> distinct=<n>
```

| Column | What it is | Which candidate it names |
|---|---|---|
| `pos`/`vel` | fold of every entity in `_vehicles` (all four buckets, stored order), `_fastVehicles` (shots), `_outVehicles` — `Position()` and `ObjectSpeed()` as bits | the divergence itself, and via `_fastVehicles` the ballistics arm of C1 |
| `cam` | the scene camera position | ties the frame hash's `cameras` slice to a tick |
| `rng` | `GRandGen.SequentialDraws()` — a cumulative counter **added to `RandomGenerator`** (`randomGen.hpp/.cpp`), one unconditional increment per sequential draw | **C1 and C2**: a differing value at tick K with matching `pos` at K−1 is a draw-count divergence at K |
| `shapes` | `Shapes.Size()` — resident models | **C3**: cold admissions move it |
| `clouds` | live cloudlet count | **C1** directly: the spawn cadence is the coupled quantity |
| `ents` | entities folded | creation/deletion divergence |
| `tms` | `Glob.time` in ms — the cross-run alignment key (affine in tick index under lockstep) | — |
| `distinct` | distinct `all` hashes so far — SIM-808's anti-vacuity rule, carried on every line so a frozen capture announces itself | — |

Diffing two runs' logs joined on `tms=` names the **first divergent tick**
and, by which column moved, the **field class** — one level below what the
frame hash can see, which is the deliverable REN-THR-005 asked for.

### Ablation evidence

`tests/unit/engine/Poseidon/Core/test_tick_state_hash.cpp` — 8 cases, 37
assertions under the tag `[determinism][tickhash][sim815]`, in
`PoseidonTests`, no game data. The model run is 96 ticks × 12 entities of
well-mixed (not periodic — SIM-813's trap) deterministic state.

**One design flaw was caught by its own test before it became a green one,
recorded because it is the reason to trust the rest.** The first version
folded the `tms` alignment key into `all`. The frozen-world case then failed
with `distinct == 96` where the world had recorded the identical state 96
times — the clock's guaranteed motion was manufacturing a fresh hash per
tick, and the anti-vacuity counter could never again read 1 on any capture,
frozen or not. The alignment key is now excluded from the fold (the comment
in `EndTick` names this), and a frozen world reports `distinct=1` on every
line, which is the vacuity signal actually working.

- **Equivalence:** two identical runs → zero divergent rows, and
  `distinct == 96 of 96` on the last row (anti-vacuity satisfied with margin;
  the required floor is `kTicks / 4`).
- **A one-ulp position nudge at tick 47** → first divergent row is tick 47,
  `pos` moved, `vel`/`cam`/`rng`/`shapes` did **not** — attribution, not just
  detection. Exactly one divergent row (the model holds no cross-tick state,
  so this also pins that the accumulators are reset per tick).
- **The same nudge at the last tick (95)** → reported at 95. The control that
  makes "first" mean first rather than "early" (SIM-808's rule).
- **One extra RNG draw from tick 23 on** → tick 23, `rng` moved, no hashed
  class implicated.
- **A frozen world** → `distinct == 1` on every line: the vacuous capture is
  visible on sight.
- **Fold discipline:** `"ab"+"c"` ≠ `"a"+"bc"` (length prefix), one empty
  item ≠ no item (count suffix), `0.0f` ≠ `-0.0f` (bits, never values).

---

## 3. The run recipe (for the session that owns the game folder)

Build this branch, deploy via `scripts\Deploy.ps1`, and note the stamped
commit next to every capture (rule 0). All comparisons below are two or three
back-to-back runs of `perf_combat` with:

```
POSEIDON_LOCKSTEP_HZ=60
POSEIDON_TICK_HASH=1
```

Extract and join:

```powershell
Select-String "TickHash" run1.log | ForEach-Object { $_.Line } > t1.txt
Select-String "TickHash" run2.log | ForEach-Object { $_.Line } > t2.txt
# join on tms=, first mismatching line names the tick and the column:
fc.exe t1.txt t2.txt | Select-Object -First 20
```

(`fc` works because under lockstep the two logs are line-aligned once the
worlds are up; if the worlds started on different absolute ticks, join on the
`tms=` key instead — `tick=` counts process-lifetime steps and may be offset.)

**Sanity gates before reading anything** (a recorder that cannot say "did not
run" is the house failure mode): every line's `distinct` must be growing in
the window read, and `ents` must be > 0 — `ents=0` rows are menu/loading
ticks and prove nothing.

Then, cheapest decisive discrimination first:

1. **One diff answers most of it.** At the first divergent line, read the
   columns:
   - `rng` differs, `pos`/`vel` still equal → a **draw-count** divergence
     (C1 or C2) strictly precedes the position divergence. `clouds` also
     differing → C1; `clouds` equal → C2 (or another sequential consumer —
     the radio is the audited one that varies its own call count).
   - `shapes` differs first (or at the same tick, with `pos` parting only
     near the admitted cell) → C3.
   - `pos` differs with `rng` and `shapes` equal at and before that tick →
     none of the three; the recorder has falsified this document's ranking
     and the field class that moved is the new lead.
2. **Confirm C1 (one extra run):** set `WGR_LOD_GOVERNOR_TARGET`'s machinery
   aside and pin the governor by config — `_minLodInvWidth == _maxLodInvWidth`
   (the range collapses; `AdjustComplexity` then cannot move `_lodInvWidth`,
   `SceneDraw.cpp:1168/:1183` break out immediately). If `rng` and `clouds`
   now stay identical across runs and the first divergence moves later or
   disappears, C1 is confirmed and quantified. If no config path exists on
   this build, the same pin is one line in `AdjustComplexity` behind an env
   var — ask this branch's author.
3. **Confirm C2 (one extra run):** run with time acceleration > 1, or with
   audio disabled, so the radio takes the deterministic
   `_pauseAfterMessage = GetDuration()` path (`AIRadio.cpp:1445/:1628-1636`).
   C2's contribution vanishes; if the first
   divergent tick moves later, C2 was live at the old tick.
4. **Confirm C3 (zero extra runs):** if `shapes=` never moves inside the
   window, the world is not streaming (or the working set is stable) and C3
   is eliminated for this mission — from the logs already taken in step 1.

**What "confirmed" buys:** each candidate has a known fix shape. C1: give the
particle sources their own RNG stream (Phase 8.2's per-system stream — the
draw count may then vary without desyncing anyone else). C2: denominate the
radio drain in `Glob.time` (the nominal-duration path already exists and is
the accelerated-time behaviour). C3: admission on a deterministic budget
(objects-per-tick, not ms-per-frame) — the streaming roadmap owns it, per
SIM-813's note.

---

## 4. What this does not do

- **It names no culprit as measured fact.** All three candidates are static
  findings with verified chains; which one parts first on `perf_combat` is
  exactly what the recipe measures, and this session could not run it —
  another session owns the game folder, and the brief forbade deploying and
  launching.
- **The recorder samples, it does not enumerate.** Positions and velocities
  of the entity lists the sim steps, one camera, four counters. A divergence
  in state it does not fold (animation phase, AI internal state, weather) is
  visible only once it moves a position. `StateTimeline` remains the tool for
  naming a *field* once the tick is known.
- **`tick=` is process-lifetime**, so two runs whose worlds start on
  different absolute ticks must be joined on `tms=`, as the recipe says.
- **The RNG counter counts, it does not attribute.** It says *how many* draws
  happened by end of tick, not who took them. If step 1 lands on "rng differs,
  clouds equal", telling the radio from another consumer needs either the C2
  ablation run or a per-site draw log, which was deliberately not built —
  a counter nobody reads is worse than none, and the two consumers whose call
  counts are timing-coupled are both already named here.
- **Uninitialised-memory reads remain unaudited** (§1.5).
- **Nothing was deployed.** The owner's game folder is untouched; the other
  session should deploy this branch itself so its captures and its binary
  match (rule 0b is its to keep, not mine to violate mid-campaign).

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/sim-div`, Bash tool
plus the SDK `mt.exe` on `PATH`, not `Build.ps1`.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| `PoseidonTests` baseline (`cc20b525`, per the brief) | 3103 | 389,910 | 0 |
| `PoseidonTests` after SIM-815 | 3111 | 389,942 | 0 |
| `PoseidonCoreTests` after SIM-815 | 838 | 9,802 | 0 |
| `PoseidonFoundationTests` after SIM-815 | 491 | 162,532 | 0 |

The `PoseidonTests` difference is exactly the 8 new cases (+32 assertions in
the suite total; the tag-filtered run reports 37, the two totals count
`REQUIRE`s in shared helpers differently). 20 cases skip in both runs
(external game data absent on this box). Core's assertion total reads 9,802
against the brief's 9,804 baseline with the same 838 cases and 0 failures —
the failure column, not the assertion column, is the stable one on the
seeded suites (SIM-813 records the same).
