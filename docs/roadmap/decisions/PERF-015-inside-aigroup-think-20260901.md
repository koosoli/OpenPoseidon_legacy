# PERF-015 — Inside `AIGroup::Think`: what the body does 15 times a tick

**Date:** 2026-09-01
**Branch:** `agent-groupthink` (off `asset-obs-001`)
**Follows:** PERF-013 (the AI stage is the engine's largest CPU cost — **read its correction
banner: its headline numbers were taken on a loaded machine and are withdrawn**), PERF-014
(inside the AI stage, and in particular its §6, the measured result), SIM-813 (the
deterministic group scan), SIM-808 (the first divergent boundary — the equivalence oracle any
change to this loop needs)
**Roadmap:** step 14 (*optimise measured hotspots*), Phase 8 step 12S (*parallelise measured
AI workloads*)

PERF-014 §6 measured, idle, on `perf_combat` after 110 s: the AI stage is 2.9–3.3 ms of a
4.7–5.3 ms tick; `groupScan` is 3.239 of it and `groupThink` 3.236, so the O(N²) selection
rescan costs **0.003 ms** and H1 is refuted; `subThink` is 0.574 and `groupExpensive` 0.108.
**2.55 ms/tick — 79% of the AI stage — is therefore inside `AIGroup::Think`'s own body**,
above the descent into subgroups and units. 15.44 group thinks a tick against ~12 groups;
**0.165 ms per group think.** PERF-014 closed by saying what the body does is "a reading
question rather than a measuring one now". This is that reading, plus the instrument that
settles it.

**This document contains no measurements.** The game folder belongs to another session. What
follows is the structural map, the instrument, and — stated in advance so the run can refute
them — what each number would mean.

## 1. The body, with file:line

`AIGroup::Think()` — `engine/Poseidon/AI/AIGroup.cpp:1201`. In source order:

| # | what | where | shape | rationed? |
|---|---|---|---|---|
| 0 | `!Leader()` → `false` | :1207 | pointer test | — |
| 0 | `TLogic` centre → `DoUpdate()`, `false` | :1213 | one FSM update | no |
| 0 | `!IsLocal()` → `CreateTargetList()`, `false` | :1222 | O(units × targets) | **yes**, `_expensiveThinkTime`, 1 Hz |
| 1 | flee/unflee: `ActualStrength()` | :1233–1263 | `MAX_UNITS_PER_GROUP` = **12** | **no** |
| 2 | `DoUpdate()` — the group FSM | :1266–1280 | one `Check` state function | **no** |
| 3 | subgroup joining | :1282–1312 | O(`_subgroups.Size()`) | **no** |
| 4 | near-enemy tracking | :1315–1342 | **O(engaged units × targets)** | **no** |
| 5 | the "expensive" block | :1346–1423 | O(units × targets) + assignment | **yes**, 1 Hz |
| 6 | combat-mode autodetect | :1425–1451 | float arithmetic | no, but constant time |
| 7 | per subgroup: `CalculateImportance()` + `Think(prec)` | :1454–1465 | per subgroup | no |

### What a *false*-returning call costs — the question PERF-014 could not answer

`Think()` returns `false` at five places, and they are **not** one population:

- **:1207** (no leader), **:1213** (`TLogic` centre), **:1222** (non-local group) return
  *before the body starts*. These are genuinely cheap — except that :1213 still runs a full
  `DoUpdate()` and :1222 still runs `CreateTargetList()` once a second.
- **:1272 / :1279** (the group was destroyed inside `DoUpdate()`) return *after* the flee
  check and the FSM.
- **:1470** — the common case named in PERF-014 — returns `false` after running **the entire
  body**. `someOperPath` is the OR of the subgroups' returns, so "this group is not currently
  operating a path" is a statement about the *result*, not about the work.

So the assumption that the 15.44 calls are "one real think and fourteen cheap ones" is not
supported by the code: a full-body think that happens to path is indistinguishable in cost
from one that does not. That has to be measured rather than inferred, which is what §2 does.

### The three unrationed walks, and the one that stands out

**(a) `ActualStrength()`** — `AIGroupImplHealth.cpp:1498`. Walks all 12 unit slots, reading
`GetArmor()`, `GetTotalDammage()` and an `InvSqrt` per unit, plus the same again for an
assigned vehicle. Runs on every full-body think of every non-player group with a live leader,
to answer one question — should this group flee — that nothing else in the body consumes and
that can only change as slowly as damage accumulates.

**(b) `DoUpdate()`** — `AIGroupImplHealth.cpp:1472` → `AbstractAIMachine::Update`
(`AICore.hpp:258`) → `FSM::Update` → one `Check` state function from `arcadeStates`
(`AIArcade.cpp:1974`, 44 states). Those range from a timeout comparison
(`CheckArcadeWait`-shaped) to a target search (`CheckArcadeGuardAttack`-shaped), so the cost
is a property of which state each group is in and cannot be bounded by reading. This is the
one segment where the structural read genuinely cannot predict the answer, which is why it
gets its own bucket rather than being folded into a "misc".

**(c) the near-enemy tracking loop** — AIGroup.cpp:1315–1342, and this is the structural
favourite. When `_nearestEnemyDist2 < Square(100)` — a firefight, i.e. `perf_combat`'s whole
premise — it calls `TrackNearTargets(_targetList)` for **every alive local unit** whose own
nearest enemy is inside 100 m. `EntityAI::TrackNearTargets` (`Target.cpp:1316`) forwards to
`TrackTargets(res, unit, canSee, false, 100, 0)` — note the **`0` period** — which walks
`res` (`Target.cpp:791`) and per entry does a distance test, a spotting-threshold decision
and `CalcVisibility` (`Target.cpp:609`), which reaches `GWorld->Visibility(brain, ai)`.

`TargetList` is a plain `RefArray<Target>` (`VehicleAI.hpp:331`) with **no size cap**:
`TargetList::Manage` (`Target.cpp:387`) keeps every entry within `TACTICAL_VISIBILITY` of any
group member. So the loop is **O(engaged units × target-list length) per full-body think**,
with the second factor a scene property rather than a constant.

And it is not rationed. `TrackNearTargets` ends by writing `_trackNearTargetsTime = Glob.time`
(`Target.cpp:1325`). **Nothing in the engine reads that field.** The only other references are
its declaration (`EntityAI.hpp:349`), its initialiser (`VehicleAI.cpp:1090`) and its
serialisation (`VehicleAIDiag.cpp:1304`). Its sibling `_trackTargetsTime` *is* read, by
`WhatIsVisible` (`Target.cpp:1364`), to ration the non-near path to once every 1–5 s. The
shape of a ration gate is present, half-built, on the one walk in the body that is neither
constant-time nor throttled. See §5.

### Ruled out by reading

- `AISubgroup::CalculateImportance()` (`AISubgroup.cpp:1469`) is `return LevelOperative;`. It
  is called per subgroup outside the `subThink` timer and PERF-014's arithmetic therefore
  charged it to the body — it is free, and it is not the 2.55 ms.
- The combat-mode autodetect (:1425–1451) is float arithmetic on members already loaded. It
  is left in the residual deliberately: if the residual is large, that is a finding, and
  bucketing it in advance would hide it.

## 2. What was instrumented, and why those boundaries

`engine/Poseidon/AI/AICostAccum.{hpp,cpp}` — **extended, not duplicated**. Same contract as
PERF-014 and `Sim::SimStageCosts()`: one process-wide accumulator, added to
**unconditionally** so there is no gate to forget, reported once a second **with its
denominators** from the same block and the same window as the two lines above it, then reset.
A zero means "ran and cost nothing"; a bucket that did not run shows a **zero count** beside
it. Six new buckets, one new nesting level:

```
groupThink                                          (PERF-014)
  |__ groupThinkEarly   exits at :1207 / :1213 / :1222
  |__ groupThinkFull    everything else, inclusive          early + full == groupThink
        |__ groupFlee        :1233-1263
        |__ groupFsm         :1266-1280
        |__ groupJoin        :1282-1312
        |__ groupTrack       :1315-1342
        |__ groupExpensive   :1346-1423                     (PERF-014, unchanged)
        |__ subThink         :1454-1465                     (PERF-014, unchanged)
```

The six inner buckets are **disjoint and all inside `groupThinkFull`**, so

```
residual = groupThinkFull - (flee + fsm + join + track + expensive + subThink)
```

is the body's unaccounted remainder — the autodetect arithmetic, `CalculateImportance()`, the
loop scaffolding and the timers' own overhead. **`residual` is printed, not left to be worked
out**, because it is the check that the segmentation is honest: a residual that is a large
share of `full` means the six boundaries do not enclose the body and nothing else on the line
should be believed. This is the same arithmetic check PERF-014 applied one level up (its seven
unnested buckets against the sim line's `ai=`), and it passed there.

### Separating the two populations

`ScopedCost` gains one method, `Retarget(Bucket)` (`AICostAccum.hpp`): it changes the
destination bucket without restarting the clock, so the scope still lands wholly in exactly
one bucket — only *which* is decided at the exit. One timer is started at function entry
aimed at `groupThinkEarly`; the moment the call gets past :1222 it is retargeted to
`groupThinkFull`. The alternative is a `ScopedCost` duplicated at each of the five `return`s,
which is the shape that silently leaks a bucket the first time somebody adds a sixth exit.

Five exit counters — `groupThinksNoLeader`, `groupThinksLogic`, `groupThinksRemote`,
`groupThinksDestroyed`, `groupThinksFull` — **sum to `groupThinks`** by construction, and all
five are printed so that stays checkable rather than asserted. `groupThinksTrue` counts the
full-body calls that returned `true`, which are the only ones that advance
`AICenter::UpdateGroup`'s loop counter (`AICenterImplPreview.cpp:549`) — so
`groupThinks − groupThinksTrue` is exactly what the walk pays to find one.

Three counters for the tracking loop: `trackScans` (full-body thinks that entered the block),
`trackCalls` (`TrackNearTargets` calls), and `trackPairs` (the sum of `_targetList.Size()`
over those calls). The third exists because `trackCalls` alone cannot tell a block that
visited 30 (unit, target) pairs from one that visited 3000, and those two want different
fixes. `groupTrack / trackPairs` is the per-pair cost; `targetsPerCall` is printed too.

### Wall time, diagnostic only — checked, not assumed

No branch, budget or selection anywhere in the AI reads any field of `CostAccum`; the only
reader is the reporting block in `World.cpp:976`. `Retarget` writes only the bucket index of a
local timer. That matters more here than anywhere else in the engine, because this is exactly
where a wall clock *did* reach a simulation decision — `UpdateGroup`'s 20 ms
`QueryPerformanceCounter` budget, which made the set of groups that think a function of CPU
speed — until SIM-813 removed it. SIM-816's determinism mode reports a divergence if one comes
back, so this is a check that can fail rather than a promise.

Behaviour is unchanged: every added scope is a pair of braces around statements that already
ran, in the order they already ran, and no control flow was altered. The `SimStageEvidence`
anchors into `AIGroup::Think` (`SimStageEvidence.cpp:137/148/161`) still resolve.

**Suites, before and after** (this worktree, `win-x64-clang-rwdi`):

| suite | before | after |
|---|---|---|
| `PoseidonTests` | 3111 cases / 389,942 assertions / 0 failures | **3111 / 389,942 / 0** |
| `PoseidonCoreTests` | 838 / 9,811 / 0 | **838 / 9,811 / 0** |
| `PoseidonFoundationTests` | 491 cases / 0 failures | **491 / 0** |

(The brief's baseline says 389,947 and ~9,808; the assertion totals of these two suites vary
run to run by a handful — `PoseidonFoundationTests` moved between 175,985 and 166,452 across
two runs of the *same* binary — because some suites are seeded randomly. **Case counts and
failure counts are the stable comparison** and both are unchanged.)

## 3. The run recipe

For the session that owns the game folder. Needs a deployed build containing commit
`a053d765` or later; `farfield-bench.ps1` refuses to run without a `DEPLOYED-FROM.txt`, which
is the right guard. **Read PERF-013's correction banner first: do not run this while anything
is compiling.** Check machine load *before* taking the measurement, not after a suspicious
result.

```powershell
.\scripts\farfield-bench.ps1 `
    -Label groupthink-body `
    -Mission tests/perf/missions/perf_combat.eden `
    -WarmupSeconds 110 `
    -TimeoutSeconds 300 `
    -Repeats 3
```

- **`-WarmupSeconds 110`** — the object stream has not settled before then; PERF-013's and
  PERF-014's numbers are all post-110 s. Any `-WarmupSeconds > 0` puts the script in timed
  mode.
- **`-TimeoutSeconds 300`** — the default watchdog is **150 s and would kill this run**.
  (Timed mode auto-computes `warmup + 180 = 290`, which also works; 300 is stated so the run
  does not depend on that default staying put.)
- **`perf_combat` only.** It is the only one of the six perf missions with meaningful AI load;
  on `perf_abel` every bucket below reads zero, correctly and uselessly. It is also the only
  one where the tracking block's `_nearestEnemyDist2 < Square(100)` gate is even open.
- **No env vars.** The accumulator is unconditional.

Then:

```powershell
Select-String -Path .tmp-farfield-bench/groupthink-body/*.log `
  -Pattern 'Sim stage ms/tick|Sim pacing|AI ms/tick|AI counts/tick|AI group body'
```

All five line kinds come from the same once-a-second block, so a set that appears together
describes the same window and the same ticks by construction.

### Gates to check before reading anything else

Three, in order. If any fails, the instrument is wrong and no hypothesis below is decidable.

1. **`aiTicks` equals the sim line's `over N ticks`.** Otherwise the AI stage was skipped on
   some ticks and every mean is over the wrong denominator.
2. **`early + full ≈ groupThink`** (within timer overhead). This is the retargeting doing what
   it claims.
3. **`noLeader + logic + remote + destroyed + full == groupThinks`** exactly, and **`residual`
   is a small share of `full`**. The first is integer arithmetic and must hold exactly; the
   second is the segmentation check.

### What would confirm or refute each hypothesis

The structural read produces four. Each says in advance what makes it *actionable* rather
than merely interesting — PERF-014's §4 defect was real, plausible, and worth 0.003 ms.

**H5 — the cost is the near-enemy tracking loop.**
*Confirmed by:* `track` carrying most of `full`, with `trackScans/tick` close to
`groupThinksFull/tick` (i.e. nearly every full-body think is in combat range) and `trackPairs`
in the thousands per tick.
*Refuted by:* `track` under ~0.2 ms/tick, or `trackScans` near zero — the latter meaning the
100 m gate is mostly shut and `perf_combat` is less of a firefight than its name suggests,
which would itself be worth recording.
*Actionable if confirmed:* yes, and it is the cheapest fix in the file — `_trackNearTargetsTime`
already exists and is already written (§5). But **only if `groupTrack / trackPairs` is small**:
a large per-pair cost points at `GWorld->Visibility` and the fix is in the visibility cache,
not here. Report both numbers.

**H6 — the cost is the group FSM.**
*Confirmed by:* `fsm` carrying most of `full`.
*Refuted by:* `fsm` under ~0.2 ms/tick.
*Actionable if confirmed:* **not directly.** The FSM is the group's behaviour; rationing it
changes when groups react, which is a gameplay change needing SIM-808's `StateTimeline` as
oracle. The actionable follow-up is a *sixth* subdivision — which `Check` state function — not
a fix. Say so rather than proposing a throttle.

**H7 — the cost is spread thin, and no segment dominates.**
*Confirmed by:* `flee`, `fsm`, `join`, `track` each 20–40% of `full`, none over half.
*Refuted by:* any one over ~60%.
*Actionable if confirmed:* the honest answer is **no**, and this is the outcome that should
close the line of enquiry rather than open another. 0.165 ms spread over four segments is a
2001 AI doing 2001 AI work, and the next lever is the *number* of thinks (15.44 a tick against
~12 groups) rather than the cost of one — i.e. `GroupsPerCycle` and the walk's termination
rule, which is SIM-813 territory and needs behavioural tests.

**H8 — the early exits are not cheap.**
*Confirmed by:* `early` being a material share of `groupThink` while `groupThinksFull/tick` is
small — meaning most of the 15.44 calls exit early but pay for a `DoUpdate()` (logic groups)
or a `CreateTargetList()` (non-local groups) on the way out.
*Refuted by:* `early` under 0.05 ms/tick, the expected case. In single-player `remote` should
be **0** by construction; if it is not, something is marked non-local that should not be, and
that is a correctness finding rather than a performance one.
*Actionable if confirmed:* yes and cheaply — an early-exiting group need not be selected by
the walk at all, and `_lastUpdateTime` is already the key.

If **none** of the four fits — the six segments sum to far less than `full`, i.e. `residual`
dominates — then the cost is in the autodetect block or in scaffolding the boundaries do not
enclose, and the next subdivision is inside `groupThinkFull` at a finer grain than the 2001
code's own seams. That would be a surprise worth its own note.

## 4. What this change does NOT prove

- **It contains no numbers.** The game folder belongs to another session. Nobody should cite
  PERF-015 for a millisecond figure until the run above has been done and this document
  amended with it.
- **It does not prove the buckets are exhaustive.** They are placed by reading; the check is
  the `residual` arithmetic in §3, which has not been performed.
- **It does not establish that `perf_combat` exercises the tracking block at all.** The 100 m
  gate is a structural read of a mission nobody has instrumented at this level. `trackScans`
  is there precisely so that is measured rather than assumed.
- **It says nothing about the FSM's state distribution.** `groupFsm` is one number over 44
  possible `Check` functions; a large value names the FSM, not the state.
- **One scene, one machine, when it is run.** Twelve groups. The counters are the same ones
  PERF-014 added so a bigger mission can be compared, not a substitute for running one.
- **It does not make the AI parallelisable.** PERF-014 §6 already refuted H3 — the work is in
  the group's own body, one call per group, twelve groups. Nothing here changes that, and
  SIM-812's finding that AI neighbour writes go past setters via friend access is unaffected
  by any timer.

## 5. A defect found and deliberately not fixed

**`EntityAI::_trackNearTargetsTime` is written and never read.**

`TrackNearTargets` (`Target.cpp:1316`) sets it on every call (`Target.cpp:1325`). The only
other references in the tree are the declaration (`EntityAI.hpp:349`, whose comment reads
"Time when function `EntityAI::TrackNearTargets` was last performed"), the constructor
initialiser (`VehicleAI.cpp:1090`) and the save-game field (`VehicleAIDiag.cpp:1304`). No
call site consults it. Its sibling `_trackTargetsTime` **is** consulted, by `WhatIsVisible`
(`Target.cpp:1364`), to ration the ordinary tracking path to once every 1–5 s.

The consequence is that the one walk in `AIGroup::Think`'s body that is O(units × targets) —
and whose target-list factor is unbounded — runs at full simulation rate, once per full-body
think, while the structurally identical walk one function away is rationed to 1 Hz. A
`Glob.time > _trackNearTargetsTime + period` guard is a two-line change and the field it needs
is already maintained.

**It is not fixed here, for four reasons.**

1. A measurement commit that also changes behaviour cannot be trusted for either. PERF-014
   made the same call and was right to.
2. **The cost is unmeasured.** PERF-014's §4 recorded an O(N²) rescan with an obvious fix that
   measurement then valued at **0.003 ms** — one tenth of one percent of the stage. That is
   the standing lesson of this line of work: a plausible optimisation is worth nothing until
   its cost is measured. H5 is the measurement, and if `track` is small this fix is pure risk.
3. **It is behaviour, not bookkeeping.** Rationing detection changes *when* units notice each
   other, which is gameplay-observable through `_targetList` ordering — and SIM-803 already
   established that this list's order is gameplay-observable, because `AssignTargets` walks it
   in order. A period long enough to save time is long enough to change a firefight.
4. **This loop is part of the simulation's definition since SIM-813.** Any change to it needs
   the behavioural and multiplayer tests the roadmap requires for an AI-ordering change, plus
   SIM-808's `StateTimeline` as the equivalence oracle.

A second, smaller one, recorded and not fixed: **`ActualStrength()` is recomputed from scratch
on every full-body think** (`AIGroupImplHealth.cpp:1498`, a 12-slot walk with two damage reads
and an `InvSqrt` per unit) to decide whether to flee — a decision that can only change as fast
as damage accumulates, and which nothing else in the body consumes. Whether that is worth
anything is H7's `flee` bucket, and the same four reasons apply to touching it.

Both are recorded here so the next session does not have to re-derive them, and neither should
be touched before the run in §3 says what they cost.
