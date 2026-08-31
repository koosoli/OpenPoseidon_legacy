# PERF-014 — Inside the AI stage: what `PerformAI` is actually made of

**Date:** 2026-08-31
**Branch:** `agent-aicost` (off `asset-obs-001`)
**Follows:** PERF-013 (the AI stage is the engine's largest CPU cost), SIM-810 (the tick's
six stages as they actually run), SIM-812 (the AI write surface), SIM-813 (the deterministic
group scan)
**Roadmap:** step 14 (*optimise measured hotspots*), Phase 8 step 12S (*parallelise measured
AI workloads*)

PERF-013 measured `World::PerformAI` at 21–34 ms/tick on `perf_combat` — the engine's
largest single CPU cost, twice the next one — and its addendum showed the catch-up cap
discarding 23–35 steps of world time a second as a consequence, so a heavy fight runs at
roughly two thirds speed. Neither could say what *inside* `PerformAI` costs that.

This is the same instrument one level down: eleven named accumulators, the counts to divide
them by, and the recipe for the session that owns the game folder to run it.

**This document does not contain measurements.** The game folder belongs to another session
today. What follows is the structural map, the instrument, and — stated in advance, so the
run can refute it — what each number would mean.

## 1. The structure, with file:line

### `World::PerformAI` — `engine/Poseidon/World/WorldImpl.cpp:451`

The whole body is guarded by `deltaT > 0` (and `!disableAI` under `_ENABLE_CHEATS`), then:

| # | what | where | shape |
|---|---|---|---|
| 1 | `GetRadio().Simulate(deltaT)` — the world radio | WorldImpl.cpp:484 | once |
| 2 | for each of `_eastCenter`, `_westCenter`, `_guerrilaCenter`, `_civilianCenter`: center radio, then `center->Think()`, then a loop simulating **every group's** radio | WorldImpl.cpp:487–526 | **per-center** and **per-group** |
| 3 | `_logicCenter->Think()` | WorldImpl.cpp:530 | once |
| 4 | the end-mission decision: a walk over **every sensor in the mission** (`sensorsMap`), counting `ASTEnd1..6` activations | WorldImpl.cpp:600 | **per-sensor**, every tick |

There are at most **five centers** — east, west, guerrila, civilian, logic — and that is a
hard ceiling, not a mission property. Everything expensive is therefore below `Think()`.

Note what item 4 is: it is not AI at all. It is the mission-end check, it runs unconditionally
once a tick, it walks the whole sensor list, and it is inside the 21–34 ms PERF-013 attributed
to the AI stage. It gets its own bucket rather than silently inflating one.

### `AICenter::Think()` — `engine/Poseidon/AI/AICenterImplPreview.cpp:557`

`TLogic` centers short-circuit to `UpdateGroup()` alone. Otherwise, in order:

1. **target-database expiry** — a walk over `_targets`, deleting entries older than
   `AccuracyLast` (7200 s) that hold no exposure. **O(targets).**
2. `RemoveOldExposures()` — AICenterImplPreview.cpp:671. Another **O(targets)** walk.
3. `UpdateMap()` — AICenterImplPreview.cpp:381. `_map->ProcessChange()` ×
   `TargetsPerCycle` (8), then a dirty-field sweep bounded by `FieldsPerCycle` (10) and 16
   rows. **Explicitly rationed by the 2001 authors** — the comment says the unrationed
   version "took 10-15% for 20fps". Note that each `ProcessChange` can reach
   `ExposureChanged` on **every subgroup of every group** (AICenterImplPreview.cpp:360–377),
   so this is `8 × groups × subgroups` in the worst case, not a constant.
4. `UpdateGuarding()` — AICenterImpl.cpp:319. **Conditional** on `Glob.time >= _guardingValid`.
5. `UpdateSupport()` — AICenterStats.cpp:1470. **Conditional** on `Glob.time >= _supportValid`.
6. `UpdateGroup()` — the group walk. See below.
7. `AddNewExposures()` — AICenterImplPreview.cpp:698. **O(targets)** again.

### `AICenter::UpdateGroup()` — `AICenterImplPreview.cpp:482` — the shape that matters

This is the one place where the structure decides what a fix looks like, and it is not what
its name suggests.

```
Determinism::GroupScanBudget budget(NGroups());     // ceiling NGroups()+1
for (int i = 0; i < GroupsPerCycle;)                // GroupsPerCycle == 1
{
    if (budget.Exhausted()) break;
    // O(NGroups) linear rescan for the oldest un-updated group
    for (int j = 0; j < NGroups(); j++) { ... maxAge/maxGrp ... }
    bool think = maxGrp->Think();                   // FULL group think
    if (think) i++;                                 // ONLY a true return advances i
    maxGrp->_lastUpdateTime = _lastUpdateTime;
}
```

`GroupsPerCycle` is **1** (AICenterImplPreview.cpp:70), so the loop stops at the first group
whose `Think()` returns `true`. But **`Think()` returning `false` does not stop it** — and
`AIGroup::Think()` returns `false` for a great many ordinary groups: no leader
(AIGroup.cpp:1207), a `TLogic` centre (1213), a non-local group (1222), a group destroyed
mid-think (1270), and — the common case — a group all of whose subgroups returned `false`,
which is every group not currently operating a path (`someOperPath`, AIGroup.cpp:1470).

So the loop's real behaviour is **"call `Think()` on groups, oldest first, until one of them
is operating a path"**, bounded only by `NGroups()+1`. Two consequences:

- The per-tick count of full group thinks is **not 1 per center**. It is somewhere between 1
  and `NGroups()+1`, and it is high exactly when few groups are pathing — which in a
  firefight, where units are stopped and shooting, is most of them.
- The selection rescan is **O(NGroups) per loop entry**, so the selection alone is
  **O(NGroups²) per center per tick** in the same case. `GroupsPerCycle == 1` reads like a
  throttle and is not one.

This is the shape the measurement has to settle, which is why `scanIters`, `groupsPerScan`
and `groupThinks` are reported beside the milliseconds rather than instead of them.

### Below the group — `AIGroup::Think()` — `engine/Poseidon/AI/AIGroup.cpp:1201`

- Flee/unflee strength check, `DoUpdate()` (the group state graph), subgroup joining.
- If any enemy is within 100 m: a `MAX_UNITS_PER_GROUP` loop calling
  `TrackNearTargets(_targetList)` per live local unit (AIGroup.cpp:1318–1341). **Per-unit,
  and it is the combat case.**
- The **expensive block**, rationed to once a second per group by `_expensiveThinkTime`
  (AIGroup.cpp:1346): `CreateTargetList()`, `AssignTargets()`, `AssignVehicles()`,
  `GetInVehicles()`, `CheckFuel/Armor/Health/Ammo`, `CheckSupport()`, `CheckAlive()`. The
  2001 authors named it expensive; whether it still is, is a measurement.
- Then, per subgroup: `CalculateImportance()` + `AISubgroup::Think(prec)` (AIGroup.cpp:1461).

### `AISubgroup::Think()` — `engine/Poseidon/AI/AISubgroup.cpp:1474`

FSM `Update` / `UpdateAndRefresh` through `AISubgroupContext`, `DoNotGo()`, formation
position/direction/coefficient refresh on a 2 s interval, an FSM `Refresh` on exposure
change, and finally a **per-unit** loop calling `AIUnit::Think(prec)` (AISubgroup.cpp:1598).
That per-unit return is what propagates up as `someOperPath` and stops the center's walk.

### The whole chain, and where the multipliers are

```
PerformAI                       ≤5 centers, +O(sensors) for the end-mission scan
 └─ AICenter::Think             O(targets) ×3, O(8 × groups × subgroups) for the map
     └─ UpdateGroup             ≤ NGroups()+1 iterations, each an O(NGroups) rescan
         └─ AIGroup::Think      O(MAX_UNITS_PER_GROUP) tracking + a 1 Hz expensive block
             └─ AISubgroup::Think   per subgroup
                 └─ AIUnit::Think   per unit  ← the leaf; OperPath lives here
```

**Nobody has counted the groups, subgroups or units a `perf_combat` tick actually visits.**
That is the gap this closes, and it is why every bucket below ships with a count.

## 2. What was instrumented, and why those points

`engine/Poseidon/AI/AICostAccum.{hpp,cpp}` — one process-wide accumulator, deliberately a
copy of `Sim::SimStageCosts()` (`World/SimStageGraph.hpp`): unconditional, so there is no
gate to forget to switch on; reported once a second **with its denominators**; reset to open
the next window. A zero therefore means "ran and cost nothing", and a bucket that did not run
shows a zero **count** beside it. A diagnostic that cannot tell those apart is a failure this
project has hit repeatedly.

Eleven timers, placed at the boundaries the 2001 code already draws rather than at every
function:

| bucket | where | why here |
|---|---|---|
| `radio` | WorldImpl.cpp:484–526 | the only per-group work in `PerformAI` itself |
| `exposure` | AICenter::Think — expiry walk + `RemoveOldExposures` + `AddNewExposures` | the three O(targets) walks, one number |
| `map` | `UpdateMap()` | separately rationed, and the only `groups × subgroups` term at center level |
| `guarding` | `UpdateGuarding()` | conditional; a zero here with a nonzero `centers` means the condition did not fire |
| `support` | `UpdateSupport()` | same |
| `groupScan` | `UpdateGroup()`, inclusive | the walk and everything under it |
| `groupThink` | `maxGrp->Think()` | **`groupScan − groupThink` is the selection rescan alone** — the O(N²) term, isolated |
| `groupExpensive` | the 1 Hz block, AIGroup.cpp:1346 | the authors' own claim, made checkable |
| `subThink` | `subgrp->Think(prec)` | per subgroup |
| `unitThink` | `unit->Think(prec)` | **`subThink − unitThink` is the subgroup FSM and formation work**, separated from per-unit AI |
| `endMission` | the `sensorsMap` walk | not AI, but inside the number PERF-013 called AI |

Four of them nest (`groupThink` ⊂ `groupScan`; `groupExpensive`, `subThink` ⊂ `groupThink`;
`unitThink` ⊂ `subThink`), so **the eleven do not sum to the stage**. The seven unnested ones
— `radio`, `exposure`, `map`, `guarding`, `support`, `groupScan`, `endMission` — do, to
within timer overhead, and that sum is what to compare against PERF-013's `ai=` figure. If it
does not match, the instrument is wrong and nothing below it should be believed.

Eight counters, so every mean has a stated denominator: `ticks` (the stage's own, counted
inside the `deltaT > 0` guard), `centers`, `radioGroups`, `scanIters`, `groupsSeen` (reported
as `groupsPerScan`), `groupThinks`, `groupExpensives`, `subThinks`, `unitThinks`.

**Wall time, diagnostic only, and this was checked rather than assumed.** No branch, budget or
selection anywhere in the AI reads these fields; the only reader is the reporting block in
`World.cpp`. That matters more here than anywhere else in the engine, because the AI stage is
exactly where a wall clock *did* reach a simulation decision — `UpdateGroup`'s 20 ms
`QueryPerformanceCounter` budget, which made the set of groups that think a function of CPU
speed — until SIM-813 removed it. SIM-816's determinism mode reports a divergence if one comes
back, so this is a check that can fail, not a promise.

Behaviour is unchanged. The one restructuring is the per-group radio loop, which `PerformAI`
repeated verbatim four times and which is now one file-local helper (`SimulateGroupRadios`,
WorldImpl.cpp:441) called from the same place in the same order — **after** the center's
`Think()`, not before it.

## 3. The run recipe

The session that owns the game folder should run this. It needs a deployed build containing
commit `75afc3bb` or later; `farfield-bench.ps1` refuses to run without a `DEPLOYED-FROM.txt`,
which is the right guard.

```powershell
.\scripts\farfield-bench.ps1 `
    -Label ai-subdivision `
    -Mission tests/perf/missions/perf_combat.eden `
    -WarmupSeconds 110 `
    -TimeoutSeconds 300 `
    -Repeats 3
```

- **`-WarmupSeconds 110`** because the object stream has not settled before then; PERF-013's
  numbers are all post-110 s. Any `-WarmupSeconds > 0` puts the script in timed mode.
- **`-TimeoutSeconds 300`.** The default watchdog is 150 s and would kill this run. (Timed
  mode auto-computes `warmup + 180 = 290`, which also works; 300 is stated so the run does
  not depend on that default staying put.)
- **`perf_combat`, not `perf_abel`.** It is the only one of the five perf missions with
  meaningful AI load — on `perf_abel` the whole fixed-step loop is 0.15 ms and every bucket
  below will read zero, correctly and uselessly.
- No env vars are needed. The accumulator is unconditional.

Then, in `.tmp-farfield-bench/ai-subdivision/*.log`:

```powershell
Select-String -Path .tmp-farfield-bench/ai-subdivision/*.log -Pattern 'Sim stage ms/tick|Sim pacing|AI ms/tick|AI counts/tick'
```

All four lines are emitted from the same once-a-second block, so a `Sim stage`, a `Sim
pacing`, an `AI ms/tick` and an `AI counts/tick` line that appear together describe the same
window and the same ticks by construction. **`aiTicks` and the sim line's `over N ticks` must
be equal**; if they are not, the AI stage was skipped on some ticks and every mean here is
over the wrong denominator.

### What would confirm or refute each hypothesis

The structural read produces four, in the order they should be checked:

**H1 — the group walk dominates, and its cost is the selection rescan, not thinking.**
*Confirmed by:* `groupScan` accounting for most of the sum of the seven unnested buckets,
**and** `groupScan − groupThink` being a large part of `groupScan`, **and** `scanIters ×
groupsPerScan` being large (thousands of comparisons per tick).
*Refuted by:* `scanIters/tick` near `centers` — i.e. the first group picked usually returns
`true` — which would mean the walk stops immediately and the O(N²) worry is theoretical.

**H2 — the walk performs many full group thinks per tick, not one per center.**
*Confirmed by:* `groupThinks/tick` substantially exceeding `centers/tick`, with `groupThink`
carrying most of `groupScan`. This is the version of H1 where the cost is real AI work rather
than the rescan, and it points at a completely different fix (rationing thinks) than H1 does
(indexing the selection).
*Refuted by:* `groupThinks/tick ≈ centers/tick`.

**H3 — the cost is per-unit, at the leaf.**
*Confirmed by:* `unitThink` carrying most of `subThink` and `subThink` most of `groupThink`,
with `unitThinks/tick` in the hundreds. This is the good outcome for Phase 8: per-unit work
under a per-group parent is the shape that parallelises, and SIM-812's audit of the AI write
surface is then the blocking question rather than an academic one.
*Refuted by:* `subThink − unitThink` dominating, which puts the cost in the subgroup FSM and
formation refresh — per-subgroup, far fewer of them, and a much worse parallel candidate.

**H4 — a large fraction of the "AI" number is not AI.**
*Confirmed by:* `endMission` or `map` being a significant share. `endMission` is the more
interesting of the two: it is a full sensor-list walk performed once a tick to answer a
question that changes seconds apart at most, and if it is material it is a defect with an
obvious fix and no AI risk attached to it.
*Refuted by:* both under a millisecond, which is the expected case and worth recording as
such.

If **none** of the four fits — the seven unnested buckets sum to far less than the sim line's
`ai=` — then the cost is in something the eleven boundaries do not enclose, and the next
subdivision is `AIUnit::Think` itself.

## 4. A defect found and deliberately not fixed

`AICenter::UpdateGroup` (AICenterImplPreview.cpp:482) selects the oldest un-updated group by
**re-scanning every group linearly on every loop entry**, and the loop entry count is bounded
by `NGroups() + 1`, not by `GroupsPerCycle`. In the case where few groups are operating a
path — a static firefight — that is **O(NGroups²) comparisons per center per tick**, to pick
a maximum out of a set that changes by exactly one element between iterations. A priority
queue keyed on `_lastUpdateTime`, or simply hoisting the scan and updating the winner, would
make it O(NGroups log NGroups) or O(NGroups) with identical selection.

It is **not fixed here**, for two reasons. A measurement commit that also changes behaviour
cannot be trusted for either. And this loop is the one SIM-813 just made deterministic; its
selection order is now part of the simulation's definition, so any change to it needs the
behavioural and multiplayer tests the roadmap requires for an AI-ordering change, plus
SIM-808's `StateTimeline` as the equivalence oracle. It also should not be fixed before H1 is
measured: if `scanIters/tick ≈ centers/tick`, this costs nothing and the fix is pure risk.

A second, smaller one, recorded and not fixed: the end-mission scan (WorldImpl.cpp:600) walks
the entire sensor list every tick to compute six activation counts. Its result can only change
when a sensor's activation changes, which is orders of magnitude rarer than 60 Hz. Whether
that is worth anything is H4.

## 5. What this change does NOT prove

- **It contains no numbers.** The game folder belongs to another session today; this is the
  instrument and the recipe, not the result. Nobody should cite PERF-014 for a millisecond
  figure until the run above has been done and this document amended with it.
- **It does not prove the buckets are exhaustive.** They are placed by reading the code, and
  the check that they are complete is arithmetic that has not been performed yet: the seven
  unnested buckets summing to the sim line's `ai=`. If that check fails, the map in section 1
  is wrong somewhere.
- **It does not establish the group/subgroup/unit counts** for a combat mission. It makes them
  measurable; the numbers in section 1 are structural bounds (`≤5` centers,
  `MAX_UNITS_PER_GROUP`, `NGroups()+1`), not observations.
- **One scene when it is run.** `perf_combat` is the only perf mission with AI load, so the
  result will describe a firefight on Everon on one machine and not the AI everywhere.
- **It says nothing about whether the AI can be parallelised.** That is Phase 8 step 12S and
  it depends on SIM-812's finding that AI neighbour writes go past setters via friend access,
  which no timer can change.
