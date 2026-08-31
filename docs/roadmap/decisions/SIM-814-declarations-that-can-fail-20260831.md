# SIM-814 — The stage graph's declarations become a gate, and six of them were wrong

**Date:** 2026-08-31
**Branch:** `agent-stage-graph`, based on `safety/asset-obs-001` at `fb58a7f9`
**Roadmap:** Phase 8, critical-path step 10S

**Related:** [SIM-810](SIM-810-the-simulation-step-as-it-actually-runs-20260831.md) (the order,
as it actually runs), [SIM-811](SIM-811-serial-task-graph-20260831.md) (the order as data,
and the declarations this audits).

SIM-810 said the value of making the tick's order data rather than a call sequence is that
*"a graph can then be asserted — that a stage's declared inputs are the ones it reads."*
SIM-811 built the graph and wrote its own honest limit:

> Honest limit: the reads/writes are hand-audited from the call sites, not derived by the
> compiler and not enforced at runtime. A stage that touches something it did not declare
> will not be caught.

A declaration nobody verifies is a comment. This is the audit, and the gate.

---

## 1. What the declarations said, and what the code does

Every finding below was reached the same way: start at the stage's entry point as
`World::StepSimulation` calls it, and follow the call chain in the source until it touches
one of the six resources. **Depth is the whole story.** Nothing at hop 0 or 1 was wrong —
SIM-811 read those correctly. Every error is at hop 2 or deeper, four of the six at hop 4 or
deeper, which is exactly where a hand-written declaration loses track.

The declarations audited are `engine/Poseidon/World/SimStageGraph.cpp:21-55` (as it stood at
`fb58a7f9`).

### Six bits were missing. In audit order:

**1. `Clock` reads `VehicleState`.** Declared `reads = 0`.

`World::SimulateLandscape` (`engine/Poseidon/World/WorldSetup.cpp:1208`) ends its weather
step with the local-airflow tick at `WorldSetup.cpp:1311`:

```
GAirflow.Update(CameraOn() ? CameraOn()->Position() : VZero);
```

That is a direct read. One hop further, `AirflowField::Update`
(`engine/Poseidon/World/Weather/AirflowField.cpp:116`) walks the helicopter list and reads
`helicopter->Position()` (`AirflowField.cpp:150`) and `helicopter->RotorSpeed()`
(`AirflowField.cpp:144`) to rank rotor wash by distance. So the **first** stage of the tick
consumes entity state the **fourth** stage writes — necessarily last tick's.

**2. `AI` writes `VehicleState`.** Declared `writes = AIState` only.

Nine hops under `World::PerformAI` (`engine/Poseidon/World/WorldImpl.cpp:429`):

| hop | file:line | call |
|---|---|---|
| 0 | `World/WorldImpl.cpp:459` | `_eastCenter->Think()` |
| 1 | `AI/AICenterImplPreview.cpp:583` | `UpdateGroup();` |
| 2 | `AI/AICenterImplPreview.cpp:532` | `maxGrp->Think()` |
| 3 | `AI/AIGroup.cpp:1450` | `subgrp->Think(prec)` |
| 4 | `AI/AISubgroup.cpp:1592` | `unit->Think(prec)` |
| 5 | `AI/AIUnitImpl.cpp:2012` | `CheckGetOut();` |
| 6 | `AI/AIUnitImpl.cpp:1408` | `ProcessGetOut(false)` |
| 7 | `AI/AIUnitImpl.cpp:848` | `DoGetOut(veh, parachute)` |
| 8 | `AI/AIUnitImpl.cpp:787` | `getOutTo->SetSpeed(veh->Speed() + ejectSpeed);` |

`AIUnit::DoGetOut` also calls `getOutTo->SetTransform(trans)` (`AIUnitImpl.cpp:788`) and
`GWorld->AddVehicle(getOutTo)` (`AIUnitImpl.cpp:790`). An AI decision to dismount moves a
person and creates an entity. Benign today because the Vehicles stage runs immediately
after — but it is a write to the resource the next stage integrates, and Phase 8's whole
point is that such a write must be declared before anything runs concurrently.

**3. `Vehicles` reads `Visibility`.** Declared `reads = Clock|AIState|VehicleState`.

Five hops under `World::SimulateAllVehicles` (`engine/Poseidon/World/WorldImpl.cpp:977`),
through the member-function-pointer dispatch:

`SimulateVehicles` (`WorldImpl.cpp:1011`) → `SimulateOnly` (`WorldImpl.cpp:690`) →
`(vehicle->*simul)(deltaT, prec)` (`WorldSetup.cpp:553`) → `Car::Simulate`
(`World/Entities/Vehicles/Ground/Car.cpp:609`) → `Transport::SimulateUnits`
(`World/Entities/Vehicles/TransportCore.cpp:907`) → `WhatFireResult` at
`TransportCore.cpp:1027` → `GLOB_WORLD->Visibility(unit, target.idExact)` at
`World/Detection/TargetFire.cpp:1317` → `_sensorList->GetVisibility(me, ai)` at
`World/WorldImpl.cpp:1053`.

Automatic fire reads the same previous-tick sensor results the AI does. SIM-810 named the
AI's visibility lag as *"the one worth investigating"*; there are two consumers of it, not
one, and only one was recorded.

**4. `Vehicles` reads and writes `PhysicsState`.** Declared neither.

`Thing::Simulate` (`engine/Poseidon/World/Scene/Thing.cpp:80`) calls
`LooseObjects::Step(*this, _physicsBody)` at `Thing.cpp:109`, and
`Poseidon::LooseObjects::Step` (`engine/Poseidon/World/Physics/LooseObjects.cpp:50`)

* spawns a solver body — `world->SpawnModelProbe(...)` (`LooseObjects.cpp:91`),
* reads its pose back — `world->GetBodyMotion(body, motion)` (`LooseObjects.cpp:100`),
* and writes it onto the entity — `thing.SetPosition(motion.position)` (`LooseObjects.cpp:108`).

`ShotShell::TryPenetrate` (`engine/Poseidon/World/Entities/Weapons/Shots.cpp:742`) also casts
against the collider set (`physics->MeasureThickness`, `Shots.cpp:762`) from inside the same
stage. **This one has an ordering consequence** — see §5.

**5. `Vehicles` reads and writes `ScriptState`.** Declared neither.

`IlluminatingShell::Simulate` (`engine/Poseidon/World/Entities/Weapons/Shots.cpp:1642`) does
this at `Shots.cpp:1665-1667`:

```
Script* script = new Script(name, arguments);
GWorld->AddScript(script);
GWorld->SimulateScripts();
```

A flare runs `onFlare.sqs` **from inside the vehicle stage**. "Scripts are a stage" is not
the whole truth, and this is not a stale comment — see §5.

**6. `Visibility` reads `Clock` and `AIState`.** Declared `reads = VehicleState` only.

`SensorList::UpdateCell` (`engine/Poseidon/World/Terrain/Visibility.cpp:334`) stamps every
cell it writes with `info.SetLastVisible(Glob.time)` (`Visibility.cpp:389`), and
`SensorList::CheckPos` (`Visibility.cpp:508`) retires a dead sensor only when nothing
commands it — `veh->IsDammageDestroyed() && !veh->CommanderUnit()` (`Visibility.cpp:538`).

### One bit is over-declared, and was kept

`Scripts` declares that it reads `Visibility`. A grep over `engine/Poseidon/Game/Commands`
for `GLOB_WORLD->Visibility`, `GetSensorList` and `GetVisibility` returns **nothing**: the
only script-side proximity of the two is `GrpKnowsAbout`
(`Game/Commands/GameStateExtGrp.cpp:1024`), which reads `grp->FindTarget(veh)` — AI state,
not sensor results.

It was **not removed.** SIM-811's rule stands: an over-declared read produces a spurious edge
somebody investigates, an under-declared one produces a missing edge nobody notices, and the
SQF command table is several hundred entries wide — the absence of a grep hit is weak
evidence over a corpus that size. It is now recorded as a **named widening** with that
reason (`SimStageEvidence.cpp`, `BuildWidenings()`), which is the difference between "wider
on purpose" and "nobody looked".

### Direct versus transitive

Asked for explicitly. **Exactly one** of the six findings — #1 — is visible without leaving
the stage's own function, and even there only via `CameraOn()->Position()` in the argument
to `GAirflow.Update`; the read that makes it matter (the rotor-wash ranking) is one hop
further. The chains recorded for the six are **2, 9, 7, 5, 4 and 3 hops** deep, in the order
above.

Every hop-0 and hop-1 declaration SIM-811 wrote was correct. The failure mode here is not
carelessness at the call site; it is that nobody re-walks a nine-hop chain when they edit a
comment, and until now nothing required them to.

---

## 2. What was rejected

**A runtime access recorder.** The obvious design: a thread-local "current stage", a
`SimTouch(resource)` at each chokepoint, and an assert when a stage touches something
undeclared. **Rejected because it cannot fail in CI.** No unit test in this repo constructs a
`World` — there is no game data on a build machine, and the three suites named in the roadmap
gate would never execute a single tick. The check would report zero violations while meaning
"did not run", which is the failure this project has documented four times in one night
(`docs/roadmap/decisions/` and the memory note *a-diagnostic-that-cannot-say-did-not-run*).
It would also have been unverifiable *by me*: another session owns the game folder and I was
forbidden to launch.

**Compile-time capability types** — give each stage an accessor object that structurally
cannot reach an undeclared resource. Correct in principle, and it would catch direct
accesses at build time, which is stronger than any test. **Rejected on reach and on size:**
it would require routing `Glob.time`, the sensor list, the physics world and entity
transforms through accessors across most of `engine/Poseidon`, and it *still* would not see
transitive reach without routing every intermediate layer too. Five of the six findings are
transitive.

**A computed call graph over the whole source.** A name-based BFS from each stage root
reaches essentially every function in the engine within three hops (`SetPosition` alone has
hundreds of sites), so every stage would be shown to touch every resource. Vacuous in the
permissive direction and useless in the restrictive one.

**A diagnostic that prints the discrepancies.** Explicitly out of scope: this project
prefers a closed gate to a readout, and a readout is what SIM-811 already had.

---

## 3. What was built

`engine/Poseidon/World/SimStageEvidence.{hpp,cpp}` — a checked-in **evidence table**, and
`tests/unit/engine/Poseidon/World/test_sim_stage_evidence.cpp` — ten cases, tag
`[determinism][taskgraph][simstage]`, in `PoseidonTests`. No game data, no `World`.

Each evidence entry is one `(stage, resource, read|write)` bit plus the call chain that
justifies it, hop by hop, as *file* + *enclosing definition* + *the text that must appear
inside it*. 26 entries, 3 widenings, and a census of 3 resources.

### The three gates, and what each one trips on

**LIVENESS** — every hop of every chain still exists. The test finds the enclosing
definition by its column-zero signature, brace-matches its body (skipping comments, strings
and character literals so a `{` in a log format string cannot end a body early), and
requires the recorded call text inside it. *Trips when somebody deletes, renames or moves a
call anywhere in a chain* — the actual mechanism by which a declaration goes stale.

**LINKAGE** — hop *i*'s callee name must appear in hop *i+1*'s signature, so a chain cannot
skip a link. Three hops in the table are marked `indirect` — the `(vehicle->*simul)` member
pointer and the SQF command table — and the count of them is bounded in both directions, so
a table that quietly became all-indirect fails rather than passing by skipping.

**COVERAGE** — every declared bit has a chain **or** a named widening with a reason; and
every chain's bit is declared. The two sides cannot drift apart in silence in either
direction.

**CENSUS** — for `PhysicsState`, `Visibility` and `ScriptState`, whose chokepoints are rare
enough to enumerate (`Physics::GetPhysicsWorld()`, `GetSensorList()`, `SimulateScripts()`),
**every non-comment occurrence anywhere under `engine/Poseidon`** must match a classified
site: either in-tick, naming the stage that reaches it, or out-of-tick with a reason. An
in-tick site additionally requires its stage to declare the resource. This is the gate that
catches a **new** touch nobody recorded.

### Anti-vacuity, because every gate here is a loop over a table

A first case pins the table sizes (≥25 entries, exactly 3 census resources), requires every
entry to have at least one hop and a written note, requires the deepest chain to be ≥9 hops
(so the table cannot degenerate into re-reading `StepSimulation`), and requires the source
files to be readable from the test's working directory at all. The census requires ≥4
occurrences per witness and ≥6 in-tick sites — a witness that matched nothing would make the
whole census pass while measuring nothing.

---

## 4. Ablation evidence

Five ablations, each built and run, each output recorded. **Two directions, and the check
catches both** — but by different gates, and it is worth saying which.

**A. A declared read deleted** (Visibility no longer declares it reads `VehicleState`):

```
every recorded call path corresponds to a declared bit
  REQUIRE( (DeclBit(e.stage, e.write) & SimResourceMask(e.resource)) != 0 )
  0 != 0
  A call path is recorded for stage 'visibility' reads resource 'vehicleState'
```

**B. A bit declared with nothing behind it** (Physics claims to read `AIState`):

```
every declared resource bit has a call path or a named widening
  REQUIRE( (evidenced || widened) )  ->  false
  stage 'physics' declares that it reads resource 'aiState'.
  Nothing justifies that bit: add a call path to SimStageEvidence.cpp, or a
  widening with a reason.
```

**C. Staleness — a call renamed three hops under a stage** (`SensorUpdate::SetLastVisible`
→ `SetLastSeen`, in both the header and the definition, so it still compiles):

```
every hop of every recorded call path still exists in the source
  stage 'visibility' reads resource 'clock'
    because: Sensor results are stamped with the tick's time
    hop 2 of 3: World/Terrain/Visibility.cpp  [int SensorList::UpdateCell(]
      must contain  [info.SetLastVisible(Glob.time)]
    the enclosing definition was found
    searched World/Terrain/Visibility.cpp:334 .. 434
```

Note what that says: the definition was *found*, the call was *not*. It distinguishes "the
function moved" from "the call went away", which is the distinction somebody fixing it needs.

**D. The other direction — a stage touching an undeclared resource.** A
`Physics::GetPhysicsWorld()` call added inside `SensorList::CheckPos`, i.e. the Visibility
stage reaching into the solver:

```
every occurrence of a censused witness is classified
  unclassified use of [Physics::GetPhysicsWorld()] at World/Terrain/Visibility.cpp:511
        (void)Physics::GetPhysicsWorld(); // ABLATION D
  Add it to SimResourceCensus() ... either as an InTick site naming the stage
  that reaches it, or as OutOfTick with a reason no stage does.
```

**E. …and classifying it honestly does not make the failure go away.** With D still in
place, the new call was added to the census as an in-tick site reached by the Visibility
stage — the truthful classification:

```
an in-tick census site's stage declares the resource it touches
  stage 'visibility' reaches World/Terrain/Visibility.cpp and therefore reads
  'physicsState', but does not declare it
```

D and E together are the point: the census cannot be satisfied by *describing* a new touch,
only by declaring it. A and B are the same door from the other side.

All five were reverted; the file states were restored from copies taken before the first
ablation and confirmed against `git status`.

---

## 5. Two findings that are defects, not stale comments — parked, not fixed

Per the working rule, these are reported rather than changed in the same commit. Both are in
`docs/roadmap/PARKED.md`.

**Loose objects are always one tick behind.** The Vehicles stage reads
`PhysicsWorld::GetBodyMotion` (`LooseObjects.cpp:100`) and the Physics stage steps the solver
(`World.cpp:2018`) — in that order. So a barrel's pose comes from a solver that has not
integrated this tick. The graph now reports it as `physics → vehicles (physicsState)`,
previous-tick. Whether to move the hand-over or the stage is a physics-behaviour question
with no test coverage.

**The Vehicles stage re-enters the Scripts stage.** `IlluminatingShell::Simulate` runs
`GWorld->SimulateScripts()` (`Shots.cpp:1667`) inside the vehicle stage. This dents the model
rather than the declaration: a stage is supposed to be a stage. Under Phase 8's concurrency
a script would run on whichever thread happened to be simulating a flare. Rare (a flare, with
`onFlare.sqs` present in the mission) and harmless while everything is serial.

Not parked, because it is a question rather than a defect: **the AI's `VehicleState` write**
(finding #2) is same-tick and correct today, but it means the AI and Vehicles stages are not
separable in the way "AI produces orders, Vehicles integrates" suggests.

---

## 6. Suite counts

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-stage-graph`, Bash plus the
SDK `mt.exe` on `PATH`, not `Build.ps1`.

| suite | before (`fb58a7f9`) | after | delta |
|---|---|---|---|
| `PoseidonTests` | 3093 cases / 389,191 assertions / 0 failures | 3103 / 389,905 / 0 | +10 cases, +714 assertions |
| `PoseidonCoreTests` | 827 / 6,341 / 0 | 827 / 6,341 / 0 | unchanged |
| `PoseidonFoundationTests` | 491 cases / 0 failures | 491 / 0 | unchanged |

20 cases skip in `PoseidonTests` in both runs (external game data absent), 4 in
`PoseidonCoreTests`. `PoseidonFoundationTests`' assertion total varies run to run because
some cases are randomly seeded; the case and failure counts are the stable figures.

One existing assertion changed: `test_serial_task_graph.cpp`'s
`REQUIRE(lagged.size() == 4)` became `== 8`. The four SIM-811 named are all still there; the
six new declaration bits add four more previous-tick edges. The whole set, in order, is now
pinned in `test_sim_stage_evidence.cpp`:

| producer | consumer | resource | |
|---|---|---|---|
| `vehicles` | `clock` | `vehicleState` | **new** — the wind tick reads last tick's rotors |
| `vehicles` | `scripts` | `scriptState` | **new** — the flare's script |
| `ai` | `scripts` | `aiState` | SIM-811 |
| `vehicles` | `scripts` | `vehicleState` | SIM-811 |
| `visibility` | `scripts` | `visibility` | SIM-811 (and a *widened* read — see §1) |
| `visibility` | `ai` | `visibility` | SIM-811, the one SIM-810 flagged |
| `visibility` | `vehicles` | `visibility` | **new** — automatic fire |
| `physics` | `vehicles` | `physicsState` | **new** — loose objects, and a defect (§5) |

---

## 7. What this change does NOT prove

- **It is not a measurement of a running engine.** Not one tick was executed. The gate reads
  source text. It proves the declarations agree with the code *as written*, not that the code
  does what anybody wants.
- **Three resources have no census.** `Clock`, `VehicleState` and `AIState` are touched
  through `Glob.time`, `SetPosition`, `Brain()` and a dozen other spellings at thousands of
  sites; no exhaustive enumeration is affordable and none is claimed. For those three the
  declaration is evidence-backed but **not** exhaustive, and a genuinely new touch of one of
  them will not be caught. The test states this as an assertion
  (`the census says which resources it does NOT cover`) rather than leaving it to be
  discovered.
- **Three hops are indirect and unverifiable.** The `(vehicle->*simul)` dispatch and the SQF
  command table are resolved at run time. The gate checks that both ends of those hops exist;
  it cannot check that the dispatch actually reaches the named function.
- **The chains are one path each, not all paths.** `Car::Simulate` is one of nine
  `SimulateUnits` callers; `Eject` is one SQF command of several hundred. A chain proves a
  bit belongs; it does not enumerate every route to it. That is why a deleted call fails the
  gate loudly and correctly — the fix is to record the surviving route, not to drop the bit.
- **Nothing was deployed and nothing was played.** Another session owns
  `D:\SteamLibrary\...\ARMA Cold War Assault` and is measuring in it, so `Deploy.ps1` was not
  run and the game was not launched. Whether the corrected declarations change anything an
  owner can see is not a question this change asks — no runtime behaviour changed; the
  declarations are inert data consumed by `AnalyseSerialOrder` and by tests.
- **`BACKGROUND_AI` was not audited.** `StepSimulation` substitutes
  `_secThread->FinishSecondary()` for `PerformAI` under that macro
  (`World.cpp:1981-1985`), and every AI chain here starts at `PerformAI`. The macro is off in
  the builds measured. A separate evidence set would be needed for it.
