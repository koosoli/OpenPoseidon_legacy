# PERF-019 — Inside `AIUnit::Think`: one unrationed walk, and the stock world has the longer list

**Date:** 2026-09-01 (instrument), 2026-09-02 (measured)
**Branch:** `asset-obs-001`
**Status:** measured — attribution only. The fix below is priced and NOT landed.
**Follows:** PERF-013 (the AI stage is the largest CPU cost), PERF-014 (inside the stage),
PERF-015 (inside `AIGroup::Think`), PERF-017 (the tracking ration).

## The headline, because it inverts the question this pass was asked

The pass was scoped to say whether the last unattributed AI cost — `AIUnit::Think`, 70–80 %
of the stage after PERF-017 — is a *heavy-world* property of DayZ Chernarus+ or universal.
It is universal, and the assumption was backwards: **the stock world is the heavier one for
this cost.**

The dominant term is `SelectInterestingTarget` (`AIUnitImpl.cpp:1156`), called from
`SetWatch()` for every idle soldier on every think, walking the whole group target list to
pick something for the soldier's head to turn towards. Its denominator, the group target
list length, is **193 on Chernarus+ and 456–460 on stock Everon** — constant through each
run, in every window, on every group. Everon's placed buildings are `Static` targets and sit
in every group's list (the walk skips them at `AIUnitImpl.cpp:1195`, but has to visit them to
skip them); the substituted DayZ world's objects are shape instances, not entities, so they
are never targets at all. A heavy world that is *lighter* for the AI.

The second inversion is in the combat phase: on Everon the strategic A* costs **554 µs per
counted search step**, on Chernarus+ **12–21 µs**. Mid-fight on the stock world the strategic
planner is the larger of the two terms; on Chernarus+ it is noise.

## Instrument

Level six of the PERF-013/014/015 accumulator (`engine/Poseidon/AI/AICostAccum.hpp`), same
rules: process-wide, unconditional, reported once a second with denominators, wall time
diagnostic only. The subdivision follows the seams the 2001 code already has:

```
unitThink   (AIUnit::Think inclusive — the PERF-014 pair at AISubgroup.cpp:1596)
  |__ unitThinkEarly  exits before SetWatch (non-local / dead / no subgroup); retargeted,
  |                   so early and full calls are two populations
  |__ unitThinkFull   the rest, inclusive; early + full == unitThink less one clock pair
        |__ unitAttack     SetWatch's auto-attack consideration, its four CommandSent
        |                  scans included; `attackThinks` counts entries into AttackThink
        |__ unitWatch      SetWatch's watch-mode switch, SelectInterestingTarget's walk
        |                  included; `watchSelects` / `watchPairs` are its denominators
        |__ unitExpensive  the per-unit 1 Hz block (CheckResources, away, NVG)
        |__ unitGetInOut   the three non-planning exits (CheckGetOut / CheckGetInGetOut)
        |__ unitStrat      planned-modes preamble: drift check, RefreshStrategicPlan,
        |                  CreateStrategicPath (strategic A*); `search` counts calls where
        |                  the callee's own guards pass. Marked at the Think call site
        |                  because Ship.cpp:1303 also calls CreateStrategicPath
        |__ unitOperTarget OperPath (next waypoint off the strategic plan)
        |__ unitOperPlan   CreateOperativePath (operative planner; `init` entries plan)
```

The seven leaves are disjoint and inside `unitThinkFull`; `residual = full − Σleaves` is
printed and is the check that they enclose the body. Marks were verified against what the
names claim: `SetWatch`, `OperPath`, `CreateOperativePath` have no caller outside
`AIUnit::Think`; `CreateStrategicPath` has one, hence the call-site mark. A missing
`BucketName` case for `groupThinkFull` (it printed "?") was fixed in passing.

**Behaviour-neutral by construction.** The diff is scope braces, RAII timers and counter
increments; no branch, budget or selection reads a field. The one guard added
(`grp && grp->GetCenter()` before `unitWatchPairs += Size()`) is on the probe, so that a
counter cannot dereference a group the callee tolerates being absent. The SIM-808 oracle was
therefore not run; the `--check` source-evidence gate (`SimStageEvidence.cpp:163`, which
needs `bool AIUnit::Think(` and `CheckGetOut();` in order) still passes on the edited file.

## Runs

`perf_combat.eden`, `--render=wgpu --window 1280x720`, `WGR_DLSS=0 WGR_RENDER_THREAD=0`,
no lockstep and no seed (not required: nothing touched control flow, and it means the runs
are NOT replays of one another — see the spread). Chernarus+ is `--test-world
chernarusplus.wrp --addon-root <DayZ>/Addons --test-world-freefly 5704.58 2523.37 120 80.9
-3.8 --test-world-hour 12`; Everon is the same command with those four options dropped.

| run | world | binary | rows |
|---|---|---|---|
| chern-r1 | Chernarus+ | 2026-09-01 21:19 build of the previous session's tree | 731, exited on its 2000-frame screenshot |
| chern-r2 | Chernarus+ | same | 650, killed with the session |
| **chern-r3** | Chernarus+ | **2026-09-02 00:59:47 build of this commit's tree** | 720, killed at 750 s |
| **everon-r1** | Everon | same | 663, killed at 690 s |

The 21:19 binary differs from the final one only by the probe guard above and comment text.
`dist/` staging was verified by exe mtime and by grepping the exe for the new log string
(the trap in `dist-staging-fails-silently`). Rows are the once-a-second `AI unit think
ms/tick` / `counts/tick` lines; "row 0" is the first one after gameplay entry.

**Caveat on the terminal marker.** everon-r1 and chern-r3 have no `AUTO-TEST SUCCESS` line:
they were run with `--screenshot-delay 100000` and terminated by `timeout -s KILL` at the
planned time, so the log ends mid-row. Every window below lies well inside the rows that
exist, and the settled numbers are flat for the last ten minutes of each run.

## Measurement

Means over ten consecutive rows. `ai` is the stage total from the `Sim stage` row; `pairs`
is `watchPairs`/tick; per-pair and per-select costs are `watch` divided by them.

### Settled (last 10 rows of each run)

| run | ai | unitThink (min–max) | watch | watch/unit | unit/ai | thinks | selects | pairs/sel | pairs | ns/pair | µs/sel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| chern-r1 | 0.296 | 0.260 (0.241–0.282) | 0.243 | 93.8 % | 87.7 % | 43.0 | 43.0 | 193 | 8,299 | 29.3 | 5.7 |
| chern-r2 | 0.478 | 0.434 (0.418–0.481) | 0.413 | 95.2 % | 90.8 % | 65.0 | 65.0 | 193 | 12,545 | 32.9 | 6.4 |
| chern-r3 | 0.536 | 0.493 (0.482–0.519) | 0.472 | 95.7 % | 92.1 % | 78.0 | 78.0 | 193 | 15,054 | 31.4 | 6.1 |
| everon-r1 | 0.273 | 0.225 (0.221–0.234) | 0.211 | 93.7 % | 82.4 % | 44.0 | 44.0 | 456 | 20,057 | 10.5 | 4.8 |

Everything else in the settled body is below 0.01 ms: `attack` 0.001, `expensive`
0.001–0.002, `getInOut` 0.001–0.007, `strat`/`operTarget`/`operPlan` 0.000–0.003 (the
planners have nothing left to plan — `operPlans` is 0–3 per tick). `residual` is
0.005–0.009 ms, 2–3 % of `full`, so the segmentation holds. `early + full − unitThink` is at
most 0.011 ms (0.027 on chern-r2), the advertised one clock pair per call.

**`selects == full thinks` in every window of every run**: each idle soldier runs the walk on
every think, and every unit thinks on every tick at 60 Hz. There is no ration of any kind
between this walk and the tick rate.

### Combat (rows 40–49, about 40 s after gameplay entry)

| run | sim | ai | unitThink | unit/ai | watch | strat | operPlan | attack | selects | pairs | ns/pair | search/tick | µs/search |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| chern-r1 | 2.81 | 1.347 | 0.783 | 58 % | 0.621 | 0.014 | 0.074 | 0.051 | 97.6 | 18,839 | 32.9 | 0.68 | 21 |
| chern-r2 | 2.82 | 1.196 | 0.705 | 59 % | 0.555 | 0.014 | 0.071 | 0.043 | 87.4 | 16,869 | 32.9 | 1.19 | 12 |
| chern-r3 | 2.71 | 1.196 | 0.736 | 62 % | 0.608 | 0.008 | 0.069 | 0.028 | 96.3 | 18,595 | 32.7 | 0.65 | 12 |
| everon-r1 | 13.61 | 4.849 | 3.044 | 63 % | 1.094 | **1.568** | 0.169 | 0.156 | 80.8 | 37,093 | 29.5 | 2.83 | **554** |

On Chernarus+ the watch walk is 79–83 % of the unit think at every phase. On Everon in
combat the strategic A* overtakes it: 52 % `strat` against 36 % `watch`, at 554 µs per
counted search step against 12–21 µs on Chernarus+. That window also shows what the stage
looks like when it is the engine's problem — 4.85 ms of a 13.6 ms tick, the sim shedding to
51 ticks/s — and the group-level rows say the rest of it: `track` 1.05, `groupExpensive`
0.77, `subThink` 4.22 (`AI group body` row 45). Everon's `residual` there is 0.034 ms, 1.1 %.

Mission start (rows 0–9) is the operative planner's moment: on Everon `operPlan` is 0.291 of
0.543 ms with 5.4 `init` entries per tick (54 µs per path built); on Chernarus+ 0.062 with
0.7. It is over within a minute on both.

### Spread

Three Chernarus+ runs settle at 0.26 / 0.43 / 0.49 ms of `unitThink`. That is not the
instrument's spread; it is the survivor count — 43, 65 and 78 units alive when the fight
ends, because the runs are unseeded and the outcome differs. The per-unit cost is the stable
figure: **5.7–6.4 µs per select** (29–33 ns per pair × 193) on Chernarus+, 4.8 µs per select
(10.5 ns × 456) on Everon. Per pair, Everon is cheaper because more than half its list is
statics rejected at the fourth test; per select, the two worlds are within 25 % of each
other with a 2.4× difference in list length. Within a window the row-to-row spread is
±8 % (chern-r1: 0.241–0.282) and ±3 % (everon: 0.221–0.234).

The Chernarus+ combat rows are 1.2–1.35 ms of AI stage, which is where the 1.4–2.3 figure
this pass was scoped from sits; the settled 0.3–0.5 is the same scene after the fight.

### A zero worth noting

`attackThinks` is 0 in every row of all four runs — 2,760 s of `perf_combat` and
`AttackThink()` never ran. The 0.03–0.16 ms `attack` in the combat rows is entirely its gate:
four `CommandSent` scans per full think. Whether that is the mission (targets are engaged
via group commands, so `CommandSent(Attack)` is always true when a target is assigned) or a
dead path is a behaviour question for another document; it is recorded here because a
bucket whose count is zero and whose time is not is exactly the shape rule 0d's "did not
run" note asks to be said aloud.

## The fix, priced and not landed

**Ration `SelectInterestingTarget` per unit.** The walk decides `_watchTgt` and
`_watchDirHead` — which way an idle soldier's head turns. Nothing about that needs a 60 Hz
answer, and the same function already rations its other slow work: the block ten lines
below runs on `_expensiveThinkTime` (`AIUnitImpl.cpp:1961`, `ExpensiveThinkDone()`). The
2001 comment above the call — "there is no need to simulate this for units that are far" —
describes a gate that was intended and never written, the same shape PERF-015 found on
`_trackNearTargetsTime`.

Price, from the tables: at 0.5 s per unit against a 60 Hz tick, one walk in thirty
survives.

| window | watch today | watch at 0.5 s | saved | share of the AI stage |
|---|---:|---:|---:|---:|
| Chernarus+ settled (r1 / r3) | 0.243 / 0.472 | ~0.01 / ~0.02 | 0.23 / 0.45 ms/tick | 78 % / 84 % |
| Chernarus+ combat (r1) | 0.621 | ~0.02 | 0.60 ms/tick | 45 % |
| Everon settled | 0.211 | ~0.01 | 0.20 ms/tick | 73 % |
| Everon combat | 1.094 | ~0.04 | 1.05 ms/tick | 22 % |

Implementation is one `Time` member and one comparison, staggered by unit ID so the
survivors do not all land on the same tick. It is a behaviour change — idle head-tracking
lags by up to the period, and `_watchTgt` is serialised state — so it needs what PERF-017
needed: `POSEIDON_AI_TIMELINE` on both sides to name the first divergence, and a smoke look
at soldiers turning their heads at the rationed cadence. Measurement and fix stay separate
commits; this document is the measurement.

**Not priced.** Shrinking the list (keeping `Static` targets out of `TargetList::Manage`,
`Target.cpp:387`) would roughly halve Everon's walk and do nothing for Chernarus+, and list
membership is gameplay-observable (SIM-803). Everon's 554 µs strategic search step is a
different item — the planner's per-call step budget on a world with a populated strategic
map — and is the next thing to attribute on stock worlds once this walk is rationed.

## What this does not say

- One scene, one machine, one Everon run. `perf_combat` is the only perf mission with real
  AI load (PERF-012).
- Nothing about framerate; the AI stage is inside `sim:step` and under overlap the frame does
  not wait for it (PERF-018).
- Not why Chernarus+'s list is exactly 193 or Everon's exactly 456–460; the constancy says
  each is the number of entities `TargetList::Manage` keeps for this mission on that world,
  and the difference is the placed statics, but neither number was decomposed.
