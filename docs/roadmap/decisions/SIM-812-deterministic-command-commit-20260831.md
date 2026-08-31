# SIM-812 — What the command/commit rewrite would actually cost, and the one piece of it that is startable

**Date:** 2026-08-31
**Branch:** `agent-command-commit`, based on `safety/asset-obs-001` at `87d513ac`
**Roadmap:** Phase 8 — *"Jobs should produce results or mutation commands. They should not
directly modify shared world state. Commit order must be deterministic and suitable for
replay testing."*

**Related:** [SIM-810](SIM-810-the-simulation-step-as-it-actually-runs-20260831.md) (the step
as it runs), [SIM-811](SIM-811-serial-task-graph-20260831.md) (the serial graph, whose
closing finding is this document's subject), [SIM-808](SIM-808-first-divergent-boundary-20260831.md)
(`StateTimeline`, the oracle), [SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md)
(the ordering precedent, including a comparator with a cycle).

**This is an audit and a design.** SIM-811 called the command/commit rewrite "a rewrite of the
two largest stages, deserving its own phase-sized item". It is right, and this session did not
attempt it. What is delivered is the map, the ordering key, one implemented primitive with an
ablated equivalence proof, and an honest size.

---

## 0. What already existed

Audited first, because seven roadmap items were found already-implemented in this repo today.

| Thing | State before this change |
|---|---|
| Any command buffer, deferred-mutation or commit-queue mechanism | **Nothing in the simulation.** `grep -ril "commandbuffer\|command_buffer\|MutationCommand\|DeferredMutation\|CommitQueue\|EnqueueCommand\|ApplyCommands"` over `engine/` and `tests/` returns four files: `Dev/Harness/HarnessBuiltins.cpp`, `Dev/Harness/HarnessServer.{cpp,hpp}` (the dev harness's *network* command protocol) and `WgpuRenderer/rust/src/dlss.rs` (a GPU command encoder). None is a simulation mutation queue. |
| The oracle | `Core/StateTimeline` (SIM-808) already existed and is used here as found. |
| The stage order | `World/SimStageGraph` (SIM-811) already existed; `CommitKey::stage` is its ordinal. |

So the ordering primitive is greenfield, and both things it must interoperate with already exist.

---

## 1. The mutation audit

### Coverage, stated before the findings

Two read-only sweeps traced down from `World::PerformAI` (`engine/Poseidon/World/WorldImpl.cpp:429`)
and `World::SimulateAllVehicles` (`WorldImpl.cpp:977`), and I verified every load-bearing claim
below against the source myself.

**Estimated coverage: 35–45% of distinct write *sites*, appreciably higher for distinct write
*shapes*.** Named gaps, so nobody mistakes this for a census:

- **AI side:** most of `AISubgroupFSM.cpp` (2569 lines; ~5 of ~40 state functions read),
  `AISubgroupFSMSupply.inc`, `AIGroup`'s own FSM, most of the ~40 `RadioMessage::Transmitted()`
  implementations, `EntityAI::WhatIsVisible` / `TrackNearTargets`, and the path planner
  (`AI/Path/`).
- **Vehicle side:** `Tank`/`Helicopter`/`Airplane`/`Ship`/`Parachute` `::Simulate` bodies (each
  confirmed to end in `Move(moveTrans)`, but not enumerated), `Transport::SimulateUnits` past
  `TransportCore.cpp:952`, `Man::SimulateAnimations`, `Landscape::GroundCollision` internals.

**I did not measure frequencies.** Every "how often" below is derived from reading guard
conditions and step sizes, and is labelled as such. Nothing here was profiled and the game was
not launched.

**One subagent claim I checked and rejected**, recorded because it would have picked the wrong
first slice: that `Target` records are aliased between a group's `_targetList` and the AI
center's database, making `AIGroupImpl.cpp:1150` `tar->dammagePerMinute += threat[kind]` a
cross-agent float accumulation. It is not. `AIGroupImpl.cpp:630` looks the center up through
`center->FindTargetInfo(...)`, which returns a **different type** (`const AITargetInfo*`) and is
only ever read from; the write lands on the group's own `Target`. That write is entity-local.

### The shapes

**(A) Writes the agent owns.** The overwhelming bulk, and not the problem. Representative:
`Simul.cpp:70` `_simulationSkipped += deltaT;` (per entity per tick); `Car.cpp:1375`
`_dampers[w] += damperForces[w] * deltaT;` (per car per tick per wheel);
`AIUnitImpl.cpp:1249` `_watchDir = rotY * _watchDir;` (per unit per tick).

One wrinkle: `Simul.cpp:1244` does `const_cast<Entity*>(this)->_prec = ret;` inside a `const`
method, so `CalculateImportance` is a mutating read.

**(B) Writes to a neighbour.** The shape that actually blocks parallelism.

| Site | Representative write | Frequency (estimated how) |
|---|---|---|
| `Car.cpp:1045` → `Simul.cpp:528` | `veh->AddImpulseNetAware(...)` → `_impulseForce += force;` | per collision pair per substep; guard is `GLOB_LAND->ObjectCollision` returning an overlap |
| `Collisions.cpp:1211` | `obj->IndirectDammage(shot, owner, pos, ...)` inside the cell loop at `:1173` | per explosion, fan-out ~10–100 objects |
| `TransportCore.cpp:1813` | `_driver->BasicSimulation(deltaT, prec, DriverAnimSpeed());` (+ gunner, commander, every cargo passenger) | per transport per tick, one per crew member |
| `AIGroupCmd.cpp:1436` | `unit->_vehicleAssigned = nullptr;` — a **friend-class poke past the setter** | event-only |
| `AICenterImplPreview.cpp:518` | `maxGrp->_lastUpdateTime = _lastUpdateTime;` — center stamps a group's field | per scheduled group per center tick |
| `AIUnitImpl.cpp:1959` | `u->SetAwayTime(Glob.time + ... + GRandGen.RandomValue() * 20);` over **every other unit in the group** | rare, fan-out = whole group, draws from the global RNG |

The friend-class writes matter disproportionately: there is no accessor to intercept, so a
binding conversion cannot be done by changing a setter.

**(C) Writes to globals.** Four distinct global structures, not one.

1. **The landscape grid** — `Landscape::_objects`, a 50 m-cell 2-D array of `ObjectList`.
   Reached only through `Object::Move` (`Object.cpp:740`/`:747`) → `Landscape::MoveObject`
   (`Landscape.cpp:2279`/`:2372`). Verified: `Object::Move` has exactly one override pair in the
   whole engine (`LaserTarget.hpp:33-34`), so this is a genuine chokepoint.
2. **The AI locking grid** — a *second* spatial structure, `GLOB_LAND->LockingCache()`, written
   per non-static entity per tick from `EntityAI::Simulate` (`VehicleAICombat.cpp:1076`
   `LockPosition();` → `AIUnit.cpp:95` `fld->Lock(x1, z1, soldier, true);`), plus the road network
   (`AIUnit.cpp:166` `item->Lock();`) and building path nodes (`AIUnit.cpp:207` `building->Lock(j);`).
   Early-outs if the entity moved <0.25 m (`VehicleAIPilot.cpp:1706`). **Hotter than the landscape
   relink.**
3. **The global RNG** — `GRandGen` (`engine/Random/randomGen.hpp:49`, `mutable int _seed`).
   Sequential draws from `Collisions.cpp` (×22), `Smokes.cpp` (×31), `Car.cpp`, `Tank.cpp`,
   `SoldierOldSim.cpp`, and from the AI and the radio pacing. One shared stream consumed in tick
   order by everything.
4. **The renderer's retained scene** — `Landscape.cpp:2302` `GEngine->SceneObjectMoved(obj);`,
   plus `SceneObjectCreated`/`SceneObjectRemoved`. Also `GScene->AddLight`, `GSoundScene->AddSound`,
   `GChatList.Add` (`AIRadio.cpp:1635`), and the UI map (`AIRadioImpl.cpp:241` `AddEnemyInfo(...)`).

**(D) Allocation and destruction.** Creation happens *inside* `Simulate`:
`VehicleAICombat.cpp:1901` `GLOB_WORLD->AddFastVehicle(shot);` per shot;
`Smokes.cpp:881` `GLOB_WORLD->AddCloudlet(cloudlet);` per emitter per interval (the highest-rate
allocation in the stage); `Dammage.cpp:580` `GLOB_WORLD->AddAnimal(destroyer);` per object
destroyed. Critically, `VehicleList::Add` (`WorldSetup.cpp:642-656`) is not just a list append —
it ends in `GLOB_LAND->AddObject(object)`, so **every creation is simultaneously a landscape-grid
insert**. Deletion runs through `MoveOutAndDelete` (`WorldSetup.cpp:489`), interleaved with
simulation, and `SimulateOnly` explicitly copes with the list growing under it
(`WorldSetup.cpp:539` caches `oldVehicles = vehicles.Size()`, then a second loop at `:596`).

On the AI side an agent can **delete itself mid-think**: `AIGroup.cpp:1266` guards `DoUpdate()`
with `if (safePtr->RefCounter() == 1) return false; // group destroyed`. Any deferred scheme must
replace these ad-hoc guards, not coexist with them.

**(E) The radio is already half a command queue.** `RadioChannel::Transmit`
(`Speaker.cpp:201-222`) priority-inserts into `_messageQueue` — genuinely deferred. But the
*effects* fire inside `Simulate` (`AIRadio.cpp:1576` `NextMessage()` → `Transmitted()`, which does
arbitrary B/C/D work including `IssueCommand` and `new AISubgroup()`), the drain rate is gated on
**audio playback state**, and the inter-message pause is an RNG draw (`AIRadio.cpp:1607`
`GRandGen.Gauss(0.1, 1.0, 5.0)`). The AI also reaches back into the pending buffer and edits it
(`AIGroup.cpp:1540` `msgCmd->DeleteTo(unit);`). It is a precedent for the shape, not a model to copy.

### The finding that outranks all of the above

**`AICenter::UpdateGroup` decides which groups think this tick from a wall-clock budget.**
`AICenterImplPreview.cpp:483-521`, verified verbatim:

```cpp
Foundation::SectionTimeHandle sectionTime = Foundation::StartSectionTime();
float maxTimePerCenter = 0.020;
...
    if (Foundation::CompareSectionTimeGE(sectionTime, maxTimePerCenter))
    {
        break;
    }
```

`StartSectionTime` is `QueryPerformanceCounter`. **How many groups think in a tick is a function
of real CPU time**, so the AI stage is not deterministic today, before any parallelism, and no
commit order can make it so while this stands. It is the **only** such budget in the simulation
(`grep -rn "CompareSectionTimeGE" engine/Poseidon/` returns this one call site plus the two
implementations and the declaration), which is the good news: it is one site, not a pattern.

This should be fixed — a fixed group-per-tick budget, or a budget denominated in simulated work —
**before** any command/commit work on `PerformAI`, and it needs behavioural tests because it
changes AI think rates on slow machines.

---

## 2. The commit-order question

### What the order cannot key on

Phase 8 forbids relying on scheduler order. Three obvious keys are also unusable here, and the
third is a finding of this session:

- **`Object::ID()` — no.** `WorldImpl.cpp:1760-1768` assigns `-1` to every non-primary dynamic
  vehicle (`vehicle->SetID(-1);`). SIM-803 established this.
- **`NetworkId` — no.** Assigned only by the network layer, so null for everything in
  single-player. SIM-803 established this.
- **Position in the world's entity lists — no, and this is new.** `_vehicles` is a
  `VehiclesDistributed` (`World.hpp:233`) of **four** buckets, and `DistributeImportances`
  (`WorldSetup.cpp:335-376`) moves entities between them with `tgt->Insert(vehicle); list.Delete(i);`.
  Membership comes from `Entity::CalculateImportance` (`Simul.cpp:1198-1250`), which thresholds
  distance-to-nearest-viewer against `ENGINE_CONFIG.objectsZ`. So an entity's position in the
  simulation list is a function of **camera position and the local view-distance graphics
  setting**. It is not stable across ticks and it is not the same on two clients. Any commit
  order keyed on list position would inherit that.

SIM-803's own tie-break — *the element's index in the ordered array it came from* — is therefore
sound where it was used (fixed 12-slot `_units`, a group's `_targetList`) and **not** transferable
to the world entity lists.

### The key

```
CommitKey = (stage, site, job, seq)
```

- **`stage`** — the `SimStage` ordinal from SIM-811's `kSimStageOrder`. Keeps stages from
  interleaving once they overlap in time.
- **`site`** — which write path. Groups commutative writes together and makes a replay log
  legible.
- **`job`** — the emitting job's index in a work list **enumerated deterministically before
  dispatch**. This is the load-bearing field, and the whole point is what it is *not*: not a
  worker id, not a completion ordinal, not a pointer.
- **`seq`** — the job's own monotonic emission counter, so a job that writes the same field twice
  keeps its own internal order.

The four together are unique per record, which is what makes the commit independent of the sort
algorithm's stability. `CommitQueue::HasDuplicateKeys()` exists so a caller can assert that rather
than assume it.

**The honest limit.** Setting `job` from a pre-dispatch enumeration makes the commit independent
of *scheduler* order, which is Phase 8's requirement, and reproduces serial order exactly while
the enumeration is the current list walk — which is what makes equivalence provable. It does
**not** make the order agree across clients, because the enumeration itself is the
camera-dependent list above. Cross-client agreement needs a replicated stable entity ordinal, and
introducing one changes which entity wins a tie — a gameplay-observable change requiring the
behavioural and multiplayer tests the roadmap demands. **That is a separate item and is not
started here.**

### The comparator

Written as field-by-field `<`, **containing no subtraction anywhere**. That is directly against
SIM-803, which found `return info1->_idExact - info2->_idExact;` in `AICenterImpl.cpp` — a pointer
subtraction narrowed to `int` that produced a **cycle** for operands more than 2 GiB apart, in
code that decided which group guarded which vehicle. `job` and `seq` are 32-bit ordinals and
`(int)(a.job - b.job)` would reproduce that defect exactly. The test asserts irreflexivity,
antisymmetry and transitivity over a key space that deliberately includes ordinals spanning the
2^31 boundary.

---

## 3. What was implemented, and the slice that was rejected

### The slice I did not take, and why

The landscape cell relink looked like the obvious first conversion and I rejected it on evidence.
It has every property you want — one chokepoint (`Object::Move`, verified: one base pair plus one
override pair engine-wide), a same-cell fast path already separating the transform write from the
relink (`Landscape.cpp:2299`), tail-append and order-preserving delete
(`Landscape.cpp:2875`/`:2909`), and an estimated 1–3% of `Move` calls for ground units.

**But `Landscape::_objects` is read back within its own stage.** `Collisions.cpp` reads
`_objects(x, z)` at nine or more sites (`:167`, `:221`, `:268`, `:703`, `:1179`, `:1272`, `:1392`,
`:1493`, `:1978`), and `Landscape::ObjectCollision` is called from `Car::Simulate` while other
entities are still to be simulated. Deferring the relink to a commit point means an entity
simulated later does not see an entity that already moved into its cell — **a behaviour change,
not a refactor**. The brief and the roadmap both require observable identity.

That generalises into the rule that shapes the whole phase: **a write can be deferred without
changing behaviour only if nothing reads it back before the commit point.** By that test, most of
what the audit found — the landscape grid, the AI locking grid, impulse accumulators, target
assignment — cannot be sliced this way at all. They need the read side moved to double-buffered
inputs at the same time, which is why this is phase-sized rather than incremental.

### What was implemented

`engine/Poseidon/Core/CommitQueue.{hpp,cpp}` — the key, the comparator, and a queue that sorts
into the total order and applies. ~90 lines of code, std-only, no dependency on the World.

**It is deliberately unwired. Nothing in the engine enqueues into it.** Given the rejection above,
wiring it to a real site could not have been shown observably identical without the game folder,
which another session owns. The header says so at the top so nobody finds it later and assumes it
is live.

`tests/unit/engine/Poseidon/Core/test_deterministic_commit.cpp` — 7 cases, 146,332 assertions, tag
`[determinism][commit]`, in `PoseidonTests`. No game data.

The equivalence case runs a model of a stage twice: once mutating shared state in place in
work-list order, once by enqueuing commands **whose arrival order is shuffled** and committing
them, compared with SIM-808's `CompareTimelines`. Bit-identical for four shuffle seeds, `RunHash`
agreeing, with `DistinctTickHashes() == 64` required as SIM-808's anti-vacuity rule.

**The shuffle is the point.** A test that enqueued in work-list order would pass with no sort at
all.

---

## 4. Ablation evidence

Four ablations, each forced to fail, harness output recorded.

```
committing shuffled commands reproduces in-place mutation bit for bit
  -1 == 999999
    shuffle seed 1: no divergence          <- the equivalence really is an equivalence

ABLATION: applying in arrival order instead of committing diverges
  0 == 999999
    first divergence at tick 0, entry 0: state[0].v (continuous)
    0x3E9C0000 vs 0xBF440000 (0.304688 vs -0.765625), 2111832064 ulp

ABLATION: dropping seq from the key loses a job's internal write order
  0 == 999999
    first divergence at tick 0, entry 1: state[1].v (continuous)
    0x3F450000 vs 0x3F6D0000 (0.769531 vs 0.925781), 2621440 ulp

ABLATION: a perturbed value names its own tick and target
  47 == 999999
    first divergence at tick 47, entry 3: state[3].v (continuous)
    0x3EFED7ED vs 0x3EFED7EC (0.497741 vs 0.497741), 1 ulp
```

All four restored; the suite is green.

**Two of these caught wrong beliefs of mine before they became green tests**, which is the only
reason I trust the third:

1. The first model produced **41 distinct tick hashes out of 64**. `state = state * 0.5 + v` is a
   decaying filter and my input was periodic, so the state went periodic — a run that would have
   agreed with a wrong implementation on 23 of its ticks. Fixed with a monotone drift term.
2. **The `seq` ablation first passed as "no divergence", wrongly suggesting `seq` was
   unnecessary.** Each job's writes went to *different* slots, and writes to distinct slots
   commute, so their relative order could not matter. The model was changed so every write of one
   job targets the same slot. Writes that do not collide cannot detect an ordering error — which
   is also the reason a real conversion must be sliced by *slot contention*, not by call count.
3. The perturbation ablation first perturbed a job's *first* write and reported no divergence: five
   later writes to that slot attenuate one ulp by 0.5^5 and it rounds away. Moved to the last
   writer of the last job to reach that slot. Recorded in the test.

---

## 5. What the full conversion would cost

Rough, and derived from the audit's coverage, not from having done any of it.

| Piece | Size | Can it land incrementally? |
|---|---|---|
| Remove the wall-clock AI budget (`AICenterImplPreview.cpp:518`) | **Small** — one call site | **Yes, and it must go first.** Needs behavioural tests; changes think rates on slow machines |
| A replicated stable entity ordinal (to replace `ID()`/`NetworkId`/list position) | **Medium** — touches entity creation, serialization and the network layer | Yes, standalone; but it changes tie outcomes, so it needs the MP tests |
| Output-only writes (`SceneObjectMoved`, sound, chat, UI map) | **Small–medium** | **Yes.** Nothing in the simulation reads them back; this is the one class that is genuinely incremental |
| Deterministic RNG ownership (`GRandGen`) | **Medium** | Partly — Phase 8.2 already split positional from sequential |
| The two spatial grids (`Landscape::_objects`, the AI locking grid) | **Large** | **No.** Read back within the stage; the write side and the read side must convert together, i.e. double-buffered inputs |
| Neighbour writes in AI (`PerformAI`) | **Large** — and the audit only saw ~40% of it | No |
| Neighbour writes in vehicles (collision impulse, damage fan-out, crew) | **Large** | No |
| Allocation/destruction during simulation | **Large** — and it interacts with the self-deletion guards | No |

Sequenced honestly: the wall-clock budget and the output-only writes are startable now and are
worth doing. The stable ordinal is startable and is a prerequisite for MP determinism. Everything
else is one large piece per stage, because the read side and the write side of each shared
structure have to move together — and that, not the queue, is what makes this phase-sized.

---

## 6. What this does not do

- **Changes no gameplay ordering and no behaviour.** `CommitQueue` is unwired; the only engine
  file added is a new translation unit nothing calls.
- **Does not make SIM-811's resource declarations binding.** That was the goal and it is not met;
  this delivers the ordering primitive it would rest on, and the reason the obvious first slice
  does not work.
- **Is not a complete audit.** Coverage is stated in §1 and is 35–45% of sites.
- **Measures nothing in the running game.** No profiling, no frequency measurement. The game was
  not launched and `tri` was not run — another session owns the game folder.
- **Nothing was deployed.** The owner's game folder does not have this build and does not need it:
  no engine behaviour changed.

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-commit`, Bash tool plus the SDK
`mt.exe` on `PATH`, not `Build.ps1`. `PoseidonGame` also builds and links clean.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`87d513ac`) | 3061 | 242,709 | 0 |
| after SIM-812 | 3068 | 389,041 | 0 |

The difference is exactly the 7 new cases and 146,332 new assertions — the bulk of them the
exhaustive antisymmetry and transitivity loops over the key space. 20 cases skip in both runs
(external game data absent on this box).
