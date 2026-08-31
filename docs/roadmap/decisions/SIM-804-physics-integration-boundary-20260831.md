# SIM-804 — The physics boundary is already single-threaded and one-way; the audit's negative controls found two other bugs (2026-08-31)

Roadmap **Phase 8.4, "Physics integration boundary"**. Branch `agent-physics-boundary`,
based on `safety/asset-obs-001`; baseline measured at `01983419`, rebased twice during the
session and landed on `58c6d33e`+.
**Related:** [SIM-801](SIM-801-worker-fp-environment-20260831.md) (worker FP environment),
[SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md) (stable iteration),
[SIM-806](SIM-806-worker-count-determinism-harness-20260831.md) — whose hashing method this
reuses rather than reinventing.

## Decision

**No restructuring.** The ownership boundary 8.4 asks for already exists, is documented in
the source, and is enforced by the shape of the interface rather than by a convention
someone has to remember. Nothing crosses it from another thread, and there is nothing to
gate because there is no concurrent access to gate.

What this work adds is **evidence** — a measurement of the one number 8.4's rule depends on,
and a harness that fails if the single-threaded assumption ever stops being true — plus two
defect fixes the audit's negative controls turned up on the way.

## First, a correction to the brief

The brief said "Bullet physics is in the tree". It is not. The backend is **box3d**
(`vcpkg.json`, `"box3d"`; `engine/Poseidon/World/Physics/Box3DBackend.cpp:10`,
`#include <box3d/box3d.h>`) — Erin Catto's 3D sibling to Box2D, at version 0.1.0. The
`B3_ENABLE_ASSERT` in the build (confirmed on the clang-cl command line) is box3d's `b3_`
macro, not Bullet's `b3` from bullet3's Bullet3Common. There is no Bullet anywhere in the
tree. This matters because the two libraries have opposite threading defaults, and the
question 8.4 asks is entirely about which one is in front of you.

## The boundary as it is today

### Where the step happens

One step per fixed simulation tick, on the main thread, with no accumulator of its own:

- `engine/Poseidon/World/World.cpp:1963` — `physics->Step(deltaT)`, the last statement of
  `World::StepSimulation`.
- `engine/Poseidon/World/World.cpp:1915` — `World::StepSimulation`, whose only caller is
- `engine/Poseidon/World/World.cpp:912` — the fixed-step loop in `World::Simulate`:
  `_simulationSteps.Advance(deltaT)` returns a tick count, and `StepSimulation` runs that
  many times.

The comment at `World.cpp:1944` states the intent and it is the correct one: *"Deliberately
NOT its own accumulator — a second clock would drift against this one and reintroduce the
frame-rate dependence the whole ARCH-001 item removed."* Physics therefore inherits the
catch-up cap, the pause behaviour and `IsSimulationEnabled()` for free. The stats the
accumulator publishes are `Dev/Diag/FixedStepStats.{hpp,cpp}` via
`PublishFixedStepFrame` at `World.cpp:924`.

Two things run at **frame** rate rather than tick rate, immediately above the loop
(`World.cpp:899-901`): `Dev::ReportPhysicsCost`, `Dev::AutoDropPhysicsProbes` and
`Dev::UpdatePhysicsProbeWind`. That is deliberate and commented — probes are drawn per frame
because drawing at tick rate blinks at any frame rate that is not exactly 60. It does mean
probe *spawning* and *wind* are frame-rate dependent, which is fine for a dev probe and
would not be for anything authoritative.

### Which direction data flows

The interface is asymmetric on purpose, and says so. `PhysicsBackend.hpp:63-66`:

> *Transforms flow IN only (PHY-010). There is deliberately no getter: adding one is how
> "physics is a query service" quietly becomes "physics owns the position".*

There are exactly **three** readbacks, and each is scoped:

| Readback | Declared | Scope |
|---|---|---|
| `GetProbeSamples` | `PhysicsBackend.hpp:83-86` | probe bodies only, for debug drawing |
| `GetBodyMotion` | `PhysicsBackend.hpp:87-89` | bodies the caller spawned as dynamic |
| `CastRay` / `RayHitAnything` / `MeasureThickness` | `PhysicsBackend.hpp:80,81`; `PhysicsWorld.hpp:119` | query, not ownership |

**Exactly one place lets the solver move a gameplay entity**:
`engine/Poseidon/World/Physics/LooseObjects.cpp:108-112` writes `SetPosition` / `SetOrient` /
`SetSpeed` onto a `Thing` from `GetBodyMotion`. It is called from
`engine/Poseidon/World/Scene/Thing.cpp:109`, is **off by default**
(`LooseObjects.hpp:33`, `enabled = false`, seeded from `POSEIDON_LOOSE_OBJECTS`), and refuses
non-local objects (`LooseObjects.cpp:63`) so multiplayer keeps one authority per object.

Its ordering is worth stating because it is easy to get wrong and is right here:
`SimulateAllVehicles` (`World.cpp:1936`) runs **before** `physics->Step` (`World.cpp:1963`),
so a `Thing` reads the pose the *previous* tick produced. A consistent one-tick lag, not a
race.

### Every caller of the physics world

`GetPhysicsWorld` / `EnsurePhysicsWorld` is called from six files and nowhere else:

| Caller | Thread |
|---|---|
| `World/World.cpp:1947,1963` | main — `World::StepSimulation` |
| `World/Physics/LooseObjects.cpp:43,69` | main — `Thing::Simulate` / `~Thing` |
| `World/Entities/Weapons/Shots.cpp:744` | main — `ShotShell::TryPenetrate` |
| `Dev/Diag/PhysicsProbe.cpp` (8 sites) | main — dev panel and scene draw |
| `Dev/Diag/PhysicsCorpus.cpp:85,112` | main — deferred to first simulated frame, `World.cpp:886` |
| `Dev/Diag/PhysicsTab.cpp:71`, `PhysicsRayAudit.cpp:60` | main — ImGui dev panel |

### So: does anything touch physics off-main?

**No.** Established three ways:

1. The caller table above — every entry is on the main thread.
2. `World/` contains **no** `TaskPool::ParallelFor` or `Submit` outside `Terrain/`
   (the one exception is a fire-and-forget audio manifest preload at `WorldInit.cpp:760`).
   SIM-806 independently established that terrain generation is the *only* production
   `ParallelFor` in the Poseidon library.
3. The AI secondary thread is **compiled out**: `BACKGROUND_AI` is `#define`d to `0` at
   `World/WorldInit.cpp:90` and `World/WorldSetup.cpp:163`, so the `#if BACKGROUND_AI`
   branch at `World.cpp:1931` never builds and `PerformAI` runs inline.

`Box3DBackend.cpp` contains no `thread`, `mutex`, `atomic` or task type — verified by grep
over the whole file.

### And the backend's own threads

This is the one fact 8.4's rule actually turns on, and it was unmeasured. box3d's
`b3WorldDef` carries a `workerCount` (`box3d/types.h:186`), clamped to `[1, B3_MAX_WORKERS]`,
and the header states that a value above 1 makes the library *"create threads and use an
internal scheduler"*. The engine takes `b3DefaultWorldDef()` unmodified
(`Box3DBackend.cpp:95`) and never inspected the result. A backend quietly running its own
pool would be invisible from `PhysicsWorld` and absent from SIM-801's thread table — its FP
environment would be nobody's.

`PhysicsStats` now carries `workerCount`, read from the **live** world via
`b3World_GetWorkerCount` rather than from the default def, because
`b3World_SetWorkerCount` can change it after creation and the question is what a step will
actually do.

**Measured: 1.** The solver runs entirely on the calling thread.

## Hazards: real, latent, or absent

| # | Hazard | Verdict |
|---|---|---|
| H1 | Concurrent query of the physics world from a worker | **Absent today.** No off-main caller exists (three independent checks above). Nothing to gate. |
| H2 | Backend spawning unmanaged threads inside `Step` | **Absent, now pinned.** `workerCount == 1`, asserted. |
| H3 | Worker FP environment reaching the solver | **Latent.** Physics never runs on a worker today. Measured: box3d produced identical bits with FTZ+DAZ on and off on this fixture — see below. |
| H4 | Frame-rate-dependent physics input | **Real but non-authoritative.** Probe drops and wind are per frame (`World.cpp:899-901`), by design. No gameplay state depends on either. |
| H5 | `EnsurePhysicsWorld()` is a non-atomic lazy init | **Latent.** `PhysicsWorld.cpp:401-403` is a plain `unique_ptr` null-check. Correct while only the main thread calls it, which is all six callers. Not changed — adding a mutex would imply a concurrency that does not exist and is not wanted. |
| H6 | `RemoveBody` left a dangling entry in `_probes` | **REAL, behind a flag. Fixed.** |
| H7 | `CastRay` read the generation counter as a live/dead parity flag | **REAL. Fixed.** |

### H3, stated precisely

The load-bearing property — a worker that calls `ApplyMainFpEnvironment()` reproduces the
main thread's physics bit for bit — is asserted unconditionally. Whether box3d's solver is
*itself* sensitive to FTZ/DAZ is **measured and recorded here, not asserted**: on this
fixture the unconformed thread produced the same hash
(`8041687887682301127`) as a main thread deliberately put into flush-to-zero. That is a fact
about box3d 0.1.0 and this fixture, not a guarantee; pinning it would make the next version
of box3d responsible for keeping an accident true.

The arm is still not vacuous. A denormal sentinel runs alongside on the same thread and the
test requires it to **differ** — proving the perturbation was live — on SIM-801's principle
that a facility forcing one fixed mode would otherwise pass having tested nothing.

## The two defects, found the SIM-806 way

Both came from writing negative controls for paths nothing calls. Neither is visible in a
stock run; both are one enabled flag away.

### H6 — a removed probe body stayed in the probe list

A dynamic body lives in **two** places — the slot table and `_probes` — and `RemoveBody`
(`Box3DBackend.cpp:239`, post-fix numbering) only ever cleared the first. `Step` applies wind by walking
`_probes` (`:161`) and `GetProbeSamples` calls `b3Body_GetTransform` on every entry
(`:662`) **with no validity check**. So an individually removed probe left a dangling
`b3BodyId` that the next readback dereferenced.

Measured, three probes, remove the middle one:

| after | `bodies` | `probes` | `GetProbeSamples()` |
|---|---:|---:|---|
| spawn | 3 | 3 | 3 samples |
| `RemoveBody(ids[1])` | 2 | **3** | **SIGSEGV** |

The count was already wrong before anything crashed.

**Reachable, not theoretical.** `LooseObjects::Release` → `PhysicsWorld::Remove` is exactly
this call on a `SpawnModelProbe` body (`LooseObjects.cpp:45`), and it runs from
`Thing::~Thing` (`Thing.cpp:67`), from switching loose objects off mid-session
(`LooseObjects.cpp:56`), and from an object ceasing to be local in multiplayer (`:65`). It
had never run because `ClearProbes` — the dev panel's path — removes probes wholesale.

The quieter half is worse. Box3D reuses body ids, so once a later body took the freed id the
stale `_probes` entry **aliased** it — and `ClearProbes` destroys every body in `_probes`.
That is a building's collider deleted with nothing having asked.

**Fix:** erase the matching entry in `RemoveBody`. Erase-remove and not `std::erase_if`:
this library is built `-std:c++17`.

### H7 — `CastRay` read a generation counter as a parity flag

Slots start at `generation = 1` and `RemoveBody` increments (`Box3DBackend.cpp:284`), so a
freed slot is even — and `CastRay` required `slot.generation % 2 != 0` to name a hit. But
`Store` **reuses a freed slot without incrementing again**, so the next occupant keeps the
even generation. The ray hit; the answer came back "no body", for the entire life of that
body, after any removal anywhere in the world.

`Find()` never agreed with this reading — it compares the generation *exactly*, which is
what a generation counter is for. Two liveness tests in one file, disagreeing.

It matters because that answer feeds `ShotShell::TryPenetrate` through `MeasureThickness`
(`Shots.cpp:744`), and because every `ClearProbes()` frees slots.

**Fix:** drop the parity test. Liveness is already implied by the match itself — both
removal paths set `slot.body = b3_nullBodyId`, whose `index1` is 0, and a body Box3D just
returned from a ray hit never is.

Ablating it fails 2 assertions in 2 cases. Ablating H6's fix crashes the process.

## The harness

`tests/unit/engine/Poseidon/World/test_physics_determinism.cpp` — 8 cases, 521 assertions,
tags `[physics][determinism]`, in `PoseidonTests`. **No game data.**

Method is SIM-806's: FNV-1a over the raw IEEE-754 **bits** of the complete readback — every
`ProbeSample` *and* every `GetBodyMotion`, at every one of 240 steps, not just the final
pose. A divergence that appears at step 40 and is damped out by friction before step 240 is
still a divergence, and a final-state hash would call that run identical.

The fixture is a **cluster, not a scatter**: 24 probes of four shapes falling onto a bowl
heightfield and a static hull, so they contact each other. Bodies that never touch measure
only gravity, which any solver integrates deterministically; the contact graph is what a
worker count re-partitions — box3d's own header says exactly that of its replay player,
*"Replaying at a different count re-partitions the constraint graph"* — and what a
pointer-ordered iteration would reorder.

| Arm | Result |
|---|---|
| reference | — |
| repeated in-process, x3 | identical |
| across 512 allocations of allocator churn | identical |
| two worlds stepped interleaved | identical |
| stepped on a worker with `ApplyMainFpEnvironment()` | identical |
| read back from another thread after the step | identical |

The two worker arms are the negative control: physics is stepped from
`World::StepSimulation` and nowhere else, so "would moving it off the main thread cost
determinism" had no evidence behind it in either direction.

Two anti-vacuity guards, also SIM-806's:

- **`physics fixture is non-degenerate`** — 24/24 bodies moved, more than half rotated (so
  the *solver* acted, not just gravity), and all 24 per-body hashes are distinct.
- **`physics run hash has teeth`** — a 1 mm nudge on one of 24 bodies changes the whole-run
  hash, and so does halving the step count.

## What is NOT covered

- **No AI, scripting or network state is exercised.** This is rigid-body physics only. The
  Phase 8 gate's language about "event order, selected targets, hits, deaths, ownership
  changes or replicated state" is about jobs that do not exist yet, and none of them go
  through this boundary.
- **No worker-count sweep of the solver, because there is nothing to sweep.**
  `workerCount == 1` and the engine never sets it. If it is ever raised, `RunAndHash` is the
  function to add an arm to, and box3d ships **its own** tool for this that nothing here
  uses: `b3ValidateReplay` / `b3RecPlayer_Create` record a world and replay it at a different
  worker count, checking embedded state hashes. That is the roadmap's "first divergent
  authoritative state boundary" almost verbatim, already in the library, unwired.
- **No record/replay of our own**, and no tick-timeline bisect. The harness reports a
  divergence by failing the run comparison; it does not name the step.
- **No TSan run.** The preset is Linux-only (`linux-x64-clang-tsan`); this session is on
  Windows. Same gap SIM-806 records.
- **No cross-platform comparison and no tolerance policy.** Everything is same-build,
  same-platform, strict equality — deliberately.
- **The fixture is synthetic.** Real Geometry LODs come through `ExtractPieces`, which needs
  a `LODShape` and therefore game data. The convex-piece path *is* the production one; its
  inputs are generated. A real corpus run is what `Dev::RunPhysicsCorpus` is for and it needs
  a world loaded.
- **`MeasureThickness` and the penetration path are untested here.** H7's fix makes
  `CastRay` name recycled-slot bodies correctly, which is a precondition for penetration
  working, but nothing in this session exercised `ShotShell::TryPenetrate` end to end — that
  needs ballistics fixtures and, realistically, the game.
- **H5 was left alone.** `EnsurePhysicsWorld()` stays a non-atomic lazy init. Making it
  thread-safe would advertise a concurrency the boundary does not have and 8.4 does not want.
- **Nothing was validated in the game, and nothing was deployed.** Another session holds the
  single allowed instance, so verification here is build + unit tests. Both fixes are inert
  for every path that runs in a stock frame — H6 needs `POSEIDON_LOOSE_OBJECTS` or an
  individual probe removal, H7 needs a body in a recycled slot — which is why that is
  acceptable. **The owner's game folder does not have these fixes.**

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-phys`, targets `PoseidonTests`
and `PoseidonGame`, clean. Built with the Bash tool and the SDK `mt.exe` on `PATH`, not
`Build.ps1`.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`01983419`) | 2977 | 235,211 | 0 |
| after the two defect fixes + harness (`01983419` base) | 2987 | 235,765 | 0 |
| after rebase onto `58c6d33e`+ (other agents' work included) | 3005 | 240,328 | 0 |

The `01983419`-base difference is exactly the 10 new cases (8 determinism + 2 regression) and
554 new assertions. 20 cases skip in every run — external game data absent on this box.
