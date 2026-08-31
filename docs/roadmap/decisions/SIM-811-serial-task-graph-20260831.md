# SIM-811 — The serial task graph: the order becomes data, and the equivalence gate gets an oracle

**Date:** 2026-08-31
**Branch:** `agent-serial-task-graph`, based on `safety/asset-obs-001` at `d3d8a172`
**Roadmap:** Phase 8, critical-path step 10S — *"First run the same task graph serially. Only
after serial equivalence is proven should independent stages run concurrently."*

**Related:** [SIM-810](SIM-810-the-simulation-step-as-it-actually-runs-20260831.md) (the audit
this implements, and the source of the order), [SIM-808](SIM-808-first-divergent-boundary-20260831.md)
(`StateTimeline`, the comparison used as the oracle), [SIM-806](SIM-806-worker-count-determinism-harness-20260831.md)
and [SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md).

**This is the serial half and only the serial half.** Nothing here schedules, nothing is
thread-aware, no stage runs concurrently with any other, and no gameplay ordering changed.

## What already existed

Audited before building, because six roadmap items were investigated in this repo today and
turned out to be already implemented.

| Thing | State before this change |
|---|---|
| A task graph, stage list, or stage enum, under any spelling | **Nothing.** `grep -ril "taskgraph\|task_graph\|SimStage\|SimulationStage\|StageList"` across `engine/`, `tests/`, `docs/` returns zero hits. |
| `Core/TaskPool` | Exists, and is a **thread pool**, not a graph. Unrelated. |
| The order itself | Written down by SIM-810 the same day, as prose and a table. Not as data, and not asserted anywhere. |
| The equivalence oracle | **`Core/StateTimeline` (SIM-808) already existed and is exactly right for this.** Nothing new was needed on that side; it was used as found. |

So: the audit existed, the oracle existed, the graph did not.

## What was made explicit

`engine/Poseidon/World/SimStageGraph.{hpp,cpp}` — 6 stages, 6 resources, ~210 lines, no
dependency on anything but the standard library.

**`kSimStageOrder` is the deliverable.** SIM-810's stages 2–7, as a `constexpr` array:

```
clock → scripts → ai → vehicles → visibility → physics
```

`World::StepSimulation` now walks it. The stage bodies did not move and did not change; the
*sequence* left the function. Gating is a bitmask computed once per tick from the three real
gates — `Dev::FixedStepScriptsEnabled()`, the `updateVisibility` cheat toggle, and whether
there is a physics world — and `RunSimStagesSerial` skips a stage whose bit is clear.

The dispatch is a plain function pointer, not a `std::function`: this runs at the fixed-step
tick rate and must not allocate. It returns a `SimStageTrace` — what actually executed, in the
order it executed — so the tick's order can be *inspected and asserted* rather than inferred.
That is the whole ask of "make the order data rather than a call sequence".

### The declarations, which are the part with teeth

Each stage declares which of six coarse resources it reads and writes — `Clock`,
`ScriptState`, `AIState`, `VehicleState`, `Visibility`, `PhysicsState`. `AnalyseSerialOrder`
walks an order and reports every producer→consumer edge the declarations imply, splitting them
by whether the producer runs *earlier* in the tick (the consumer sees this tick's value) or
*later* (it necessarily sees the previous tick's).

SIM-810 said of the AI's visibility read: *"it is exactly the kind of dependency a task graph
is supposed to make explicit — under Phase 8 it becomes a declared edge, and then somebody has
to decide which way it points."* It now is one. `PreviousTickEdges(kSimStageOrder)` returns
**four**, and the test pins the whole set rather than cherry-picking the interesting member:

| Producer | Consumer | Resource | Reading |
|---|---|---|---|
| `visibility` | `ai` | `visibility` | **The one SIM-810 flagged.** The AI decides on last tick's sensor results. |
| `ai` | `scripts` | `aiState` | A script observes last tick's AI state… |
| `vehicles` | `scripts` | `vehicleState` | …last tick's positions… |
| `visibility` | `scripts` | `visibility` | …and last tick's visibility. |

The bottom three are new information and were not in SIM-810. They are the **price of running
scripts first**: the placement that wins a tick of latency on the *output* side (an order a
script issues is acted on this tick) necessarily costs a tick on the *input* side (everything
the script reads about the world is one tick stale). That trade is almost certainly still worth
it — an order arriving late is felt, a script reading a 16 ms-old position generally is not —
but it had never been written down, and now it is a number the graph reports.

**Nothing was moved.** Not the AI/visibility pair, not the scripts. Two tests exist
specifically so the order cannot be "tidied up to match the roadmap diagram" by accident:
`scripts run before the AI, deliberately` and `the AI's visibility read is a declared
previous-tick edge`.

### Honest limit on the declarations

They are hand-audited from the call sites, **not derived by the compiler and not enforced at
runtime**. A stage that touches something it did not declare is not caught. Where a stage's
reach was uncertain the declaration is deliberately *wider*: an over-declared read produces a
spurious edge, which somebody investigates and deletes, whereas an under-declared read produces
a missing edge, which nobody ever notices. Making the declaration binding is the commit stage
(below), and that is out of scope.

## How equivalence is proven, and what the proof is worth

`tests/unit/engine/Poseidon/World/test_serial_task_graph.cpp` — 15 cases, 61 assertions, tag
`[determinism][taskgraph]`, in `PoseidonTests`. No game data.

A **model of the step** is run twice and the two runs are compared with SIM-808's
`CompareTimelines`:

- **the implicit order** — a hand-written call sequence shaped exactly like the body of
  `StepSimulation` before this change, with the same conditionals in the same places;
- **the explicit graph** — the same stage bodies, driven by `RunSimStagesSerial` over
  `kSimStageOrder`.

The model is not a toy that would agree with itself whatever the order. Every stage consumes
something an adjacent stage produced, and **the AI reads `visible`, which the Visibility stage
writes later in the tick** — the real engine's lag, reproduced, because an equivalence harness
over a model without it could not tell a correct order from a reordered one.

Result: **bit-identical**, entry for entry and label for label, for all **eight** combinations
of the three real gates, and `RunHash` agrees. `DistinctTickHashes() == 64` is required in the
same cases — SIM-808's anti-vacuity rule, because a run whose ticks all hash alike would agree
with anything.

The executed stage sequence is itself recorded into the timeline as `Discrete` fields, so
**order is part of the compared state**, not a separate assertion somebody has to remember to
write.

### What this proof does and does not buy

**Buys:** the runner's semantics are established — it walks the declared order, it skips
exactly the masked stages, it neither reorders nor repeats, and the order it produces is the
order the old call sequence produced under every gate combination.

**Does not buy:** *it is not a measurement of the live `World`.* That needs game data and a
running engine, and this session was forbidden the game folder (another session owns it).
The real `StepSimulation`'s equivalence rests on two things and they should be named plainly:
the old and new code share **one** order constant that a test pins against SIM-810's audit,
and the diff is a straight lift of six statement groups into six `case` labels with no change
to any of them. That is inspection, not measurement. It is the weakest link in this record.

The one behavioural difference in the real step, stated so it is not discovered later:
`Physics::GetPhysicsWorld()` is now called when the mask is computed — at the *top* of the tick
rather than after the visibility update. It is a plain accessor over a global
(`PhysicsWorld.cpp:399`, `return g_world.get();`) with no side effects, and nothing between
those two points creates or destroys a physics world.

## Ablation evidence

**A test that cannot fail is worthless.** Five ablations were applied, each forced to fail, and
the harness's own output recorded. All five name the tick, the record and the field.

```
reordering two stages is reported (AI ⇄ visibility, order recorded)
  0 == 999999
    first divergence at tick 0, entry 1: stage[1].id (discrete)
    0x00000002 vs 0x00000004 -- a discrete outcome differs; no tolerance applies

reordering two stages changes the SIMULATED STATE, not just the label
  0 == 999999
    first divergence at tick 0, entry 5: body[0].pos (continuous)
    0xB8683117 vs 0x00000000 (-0.000055 vs 0.000000), 946352407 ulp

dropping a stage is reported as a shape change
  0 == 999999
    first divergence at tick 0, entry 4: the two runs recorded different facts in
    the same slot -- stage[4].id vs world[0].clock (an ORDERING divergence, not a
    value one)

perturbing one value at one tick names that tick, that body and that field
  41 == 999999
    first divergence at tick 41, entry 35: body[5].pos (continuous)
    0xBE97CAC7 vs 0xBE97CAC8 (-0.296469 vs -0.296469), 1 ulp

an ablation at the last tick is reported at the last tick
  63 == 999999
    first divergence at tick 63, entry 20: body[2].pos (continuous)
    0xBF4CE4C2 vs 0xBF4CE4C3 (-0.800366 vs -0.800366), 1 ulp
```

Three things worth reading out of that:

1. **The second case is the control on the first.** If the only thing a reorder changed were
   the bookkeeping record of the order, the equivalence proof would be circular. With the order
   *not* recorded, the state still diverges — 946 million ulp, because moving the AI after the
   vehicles means the vehicles integrate an aim of zero on tick 0.
2. **The last two are the control on "first".** A comparison that always answered with an early
   tick, or with the tick it was handed, would pass the tick-41 case. Moving the perturbation
   to tick 63 and requiring the answer to move with it is what makes *first* mean first. One
   ulp, at the last tick, reported as a **divergence** — no tolerance path exists to swallow it.
3. **One ablation caught a wrong belief of mine, before it became a green test.** The reorder
   case was first written asserting `d.tick > 0`, reasoning that the lagged visibility read
   needs a tick to matter. It **failed at tick 0**: the swap moves more than the lagged read.
   The assertion was corrected to what was measured, with the reasoning recorded in the test.

All five were restored and the suite is green.

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-taskgraph`, Bash tool plus the
SDK `mt.exe` on `PATH`, not `Build.ps1`. `PoseidonGame` also builds and links clean.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`d3d8a172`) | 3044 | 242,623 | 0 |
| after SIM-811 (`1624195e`) | 3059 | 242,684 | 0 |

The difference is exactly the 15 new cases and 61 new assertions. 20 cases skip in both runs —
external game data absent on this box.

One incidental finding for anyone else touching this file: the `Poseidon` library compiles as
**C++17** (`-std:c++17`), not C++20, despite the repo's C++20 framing. `std::span` is not
available; `SimStageSpan` in the header exists for that reason and says so.

## What remains before any stage can run concurrently

Phase 8's gate is *"only after serial equivalence is proven"*. Taking that literally:

**Satisfied.** The order is data. It is walked, traced, and asserted against the audit. Serial
equivalence between the explicit graph and the implicit call sequence is proved bit-for-bit by
an oracle that has been shown to fail in five distinct ways.

**Not satisfied, and honestly the more important half.** Serial equivalence being proven is not
the same as *the graph being ready to parallelise*, and Phase 8's own text makes that clear.
Four things stand between here and the first concurrent stage:

1. **The deterministic command and result commit stage — by far the largest.** SIM-810 named it
   and this session did not start it, deliberately. Phase 8 requires that jobs *"produce results
   or mutation commands"* and *"not directly modify shared world state"*. Today **every stage
   mutates in place**, and the declarations in `SimStageGraph.cpp` are therefore descriptive
   rather than binding. What it would take, concretely: a command buffer per stage with a total
   order independent of the order jobs finished in; every in-place write in `PerformAI` and
   `SimulateAllVehicles` rewritten to enqueue instead; a commit point that applies them; and a
   way to make an undeclared write fail rather than silently work. That is a rewrite of the two
   largest stages, not a refactor of the step function, and it should be its own phase-sized
   item with its own decision record.
2. **The equivalence proof must move onto the live `World`.** The model proves the runner; only
   the real step can prove the step. That needs the game folder and game data — available on
   this box, not to this session.
3. **The AI/visibility question must be settled, not merely declared.** The graph now says the
   AI reads last tick's visibility. Whether that is intended is still unknown, and SIM-810 is
   right that changing it needs behavioural and multiplayer tests first.
4. **Nothing below `StepSimulation` is covered.** `PerformAI` and `SimulateAllVehicles` are one
   stage each here and are each their own ordering problem inside — SIM-803 already found real
   defects a level down (a ranking sorted by heap address, a comparator containing a cycle).
   The parallel candidates Phase 8 lists (perception queries, sensor tests, navigation, pose
   preparation) all live *inside* those two stages, not between them. **No stage at this level
   is a parallel candidate.** The graph's value is that it now makes that visible.

## What this does not do

- **Changes no gameplay ordering.** The stages run in exactly the order they ran in before.
- **Adds no concurrency, no scheduler, no threads, no barriers.**
- **Does not enforce the declarations.** See the honest limit above.
- **Covers `StepSimulation` only** — not the debug input ahead of it, not the network, chat and
  voice handling after it, not `RenderSnapshot` (published by the caller, `World::Simulate`).
- **Nothing was validated in the game and nothing was deployed.** Another session owns the game
  folder; the game was not launched and `tri` was not run, per this session's brief. Verification
  is build plus unit tests. **The owner's game folder does not have this build and does not need
  it** — the change is a refactor of a call sequence into a walked list with no behavioural
  difference beyond the accessor-call placement noted above.
