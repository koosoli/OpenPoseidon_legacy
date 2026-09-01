# PERF-017 — What the tracking ration buys, and what it costs in behaviour

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Follows:** PERF-015 (inside `AIGroup::Think`), PERF-016 (ration before you parallelise)
**Roadmap:** step 14 (*optimise measured hotspots*), Phase 8 step 12S, Phase 13 (record/replay)

PERF-015 found that `TrackNearTargets` writes `_trackNearTargetsTime` and nothing reads it,
while its sibling `_trackTargetsTime` *is* read and does throttle its loop. PERF-016 argued
that reading it comes before any job system, and that the reason it had not been done was not
difficulty — it is one condition — but the absence of any way to say what enabling it
**changes**. Rationing an AI scan is a behavioural change and a framerate measurement cannot
see behaviour.

This closes that. Both halves are measured: the cost removed, and the behaviour moved.

## The instrument

`POSEIDON_AI_TIMELINE=<path>` records the AI's decisions into a `StateTimeline` — one entry
per unit per tick: which target it has settled on, how close it believes the nearest enemy to
be, its life state, its slot, and per group the unit and target counts. `PoseidonTools
timeline diff A B` reads two recordings and names the **first** tick, record and field at which
they stop agreeing.

`StateTimeline` has existed since SIM-808 and every consumer was a unit test, for a structural
reason: it could only compare two recordings held in one process. Two whole missions in two
processes needs a file, so it has one now (bit-exact, refuses truncation, refuses trailing
bytes — a short read would otherwise parse into a short timeline, and two short timelines
compare equal and pass the gate they were meant to fail).

**The first version of the recorder was vacuous, and only an ablation found it.** Identity was
`GetNetworkId()`, which compiled, ran, and recorded zero for every unit in the mission —
network ids are assigned by the network layer and a single-player run never assigns one. The
recording held 937,200 fields whose identity columns were constant, and two runs under
*different RNG seeds* compared EQUIVALENT. Identity is now the index at which the deterministic
sweep first meets an entity, which is stable across runs without being a pointer; `netId` now
takes 193 distinct values and `targetNetId` 13, where both took 1.

## The measurement

`perf_combat`, lockstep 60 Hz, `POSEIDON_RNG_SEED` fixed, LOD governor pinned, 902 ticks. Costs
are the `AI group body ms/tick` row over the last 60 AI ticks; `off` is the same code path with
the block disabled outright, which is the ablation.

| `POSEIDON_AI_TRACK_PERIOD` | `track` ms/tick | AI scan ms/tick | pairs/tick | first divergence from default |
|---|---:|---:|---:|---|
| **0** (today, every think) | **3.074** | **3.821** | **21,906** | — reference |
| 0.5 s | 0.267 (**−91%**) | 1.149 (**−70%**) | 1,581 | tick 332, `nearestEnemyDist2` |
| 1.0 s | 0.146 (−95%) | 0.918 (−76%) | 805 | tick 340, `nearestEnemyDist2` |
| off (ablation) | 0.002 | 0.812 (−79%) | 0 | tick 475, `nTargets` — **discrete** |

Two things follow immediately.

**Half a second buys nearly everything.** 0.5 s removes 91% of the tracking cost; going to 1.0 s
adds four more points and disabling the block entirely adds three beyond that. The curve is
almost flat past the first ration, which is the useful shape: the cheapest setting is also close
to the best one.

**The ration is not free.** Every setting diverges from the default, and at 0.5 s it diverges as
early as at 1.0 s. The ablation diverges on a *discrete* field — `nTargets`, how many targets a
group knows about — and no diagnostic may soften a discrete difference. So this is a real change
in what the AI perceives, not a rounding artefact.

## What this does and does not license

**The default is unchanged.** `POSEIDON_AI_TRACK_PERIOD` defaults to 0, which is the 2001
behaviour exactly. What the numbers establish is the trade; whether to take it is a gameplay
decision and belongs to the owner, not to a performance document.

**The oracle says "different", never "worse".** A first divergence at tick 332 of 902 is where
two runs part, not evidence that either is playing badly. Deciding that 0.5 s is *acceptable*
needs someone to watch the AI fight at that setting. That is a smoke test, and it is the one
piece of this that cannot be automated here.

**It does sharpen PERF-016's ordering.** A job system distributing 21,906 pairs a tick across
cores would be distributing work that a half-second ration removes outright. Whatever the AI
stage costs *after* this decision is the number a job system should be scoped against — and at
0.5 s the whole AI scan is 1.149 ms/tick, at which point it is no longer the engine's largest
CPU cost and step 12S should be re-argued from scratch.

## What this does not say

- One scene, one machine, one run per setting. `perf_combat` is the only perf mission with
  meaningful AI load (PERF-012), so it is the right scene and also the only one measured.
- It does not measure framerate. The AI stage is inside `sim:step`; what a 2.9 ms/tick saving
  does to a frame depends on the tick rate, and under overlap (REN-THR-016) the simulation is no
  longer the thing the frame waits for.
- It says nothing about the other loops PERF-015 lists. `subThink` is now the largest survivor
  at 0.53–0.65 ms/tick and nothing here has looked at it.
- The divergence *magnitudes* are not a severity ranking. 1.0 s parts at 1 ulp and 0.5 s at
  42,526; that is which float happened to be compared first, not which setting is gentler.

---

## Appendix — the camera as a divergence

The first same-seed pair diverged at tick 306, and the cause was the owner moving the camera
during the run. It is recorded here because it is the clearest evidence the recording is not
vacuous: an input nobody thought was part of the simulation showed up as one, at a named tick,
in a named field. It also means a recording run must be left alone, which now belongs next to
"do not measure while an agent is building" in `scripts/README-farfield-bench.md`.
