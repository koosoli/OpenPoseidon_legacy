# PERF-020 — The idle-watch target selection is rationed, and the oracle says what it changes

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** PERF-019 (which named `SelectInterestingTarget` at 94–96% of `unitThink` on both
worlds), PERF-017 (the tracking ration, same shape, same oracle)
**Authored by a subagent of this session; measured, verified and committed by the coordinator
after the agent was cut off by the session limit.** The agent's captures are in `.tmp-perf020/`.

## What changed

Every idle, non-player soldier with auto-target on ran `SelectInterestingTarget` on every
think: one pass over the whole group target list (193 entries on Chernarus+ `perf_combat`,
456 on stock Everon) to decide where his head turns. It now runs once per period per unit
(`POSEIDON_AI_WATCH_PERIOD`, default 0.5 s; `AITuning::WatchSelectPeriod`), phased by unit
slot so a group's heads do not all re-pick on the same tick. Between walks the head keeps
following the target it last chose, so the ration moves how soon a soldier notices something
*more* interesting, not whether he keeps tracking what he already looks at. 0 is the 2001
behaviour. The dev panel's AI tab has the slider, "Off (2001 behaviour)" and "Default (0.5 s)"
buttons, and live counters (`selects`, `deferred`, `pairs`). A unit's first walk is never
deferred, and the schedule time is serialized (`watchSelectTime`) so a loaded save re-picks at
once rather than inheriting a stale phase.

## Measured

AI cost rows are `ms/tick`, settled window, one machine. Unrationed rows are PERF-019's
captures on the same scenes with the same binary lineage; the oracle pair is the same scene,
same binary, one env var apart.

| scene | arm | `watch` | `unitThink` | selects / deferred per tick |
|---|---|---:|---:|---|
| Everon `perf_combat` (oracle a1) | unrationed | 0.442 | 0.639 | 102.6 / 0 |
| Everon `perf_combat` (oracle b) | 0.5 s | **0.052** | **0.184** | 6.2 / 88.4 |
| Chernarus+ `perf_combat` (PERF-019 r1–r3) | unrationed | 0.253–0.462 | 0.269–0.482 | — |
| Chernarus+ `perf_combat` (p1, settled) | 0.5 s | **0.042** | **0.062** | 2.5 / 72.5 |
| Chernarus+ `perf_combat` (p2, in combat) | 0.5 s | 0.063 | 0.325 | 5.0 / 99.0 |

On the clean same-scene pair the walk falls 88% and `unitThink` 71%. `unitThink` was 6–12% of
a 16.7 ms tick after PERF-017; it is now under 2%. The remaining `unitThink` in combat is
`attack` and `operPlan`, which are not idle work and are not rationed here.

## What the oracle says

`POSEIDON_AI_TIMELINE` diff, 902 ticks, `perf_combat`, lockstep 60 Hz, seed 1234:

- **same arm, two runs (a1 vs a2): EQUIVALENT**, 936,490 fields identical entry for entry.
  The harness can tell.
- **rationed vs unrationed (a1 vs b): DIVERGED at tick 340**, entry `unit[127].nearestEnemyDist2`,
  a continuous field, 3355 ulp apart (27683.1 vs 27689.7 m²).

That is the correct result, not a failure: the ration is a *behaviour* change by design (a head
turns up to 0.5 s later), and the first thing it perturbs is one unit's nearest-enemy distance
5.7 s in, by 6.5 m² — the soldier stood slightly differently because his head did. PERF-017's
tracking ration passed the same oracle equivalent because tracking updates a cache the
decisions read later; this one feeds a decision directly. The oracle's job is to name the
first divergent tick and field so the change is reviewable, and it did.

## What ships

Default 0.5 s, like the tracking ration, per the owner's standing instruction to build with
defaults on and keep the off switch: `POSEIDON_AI_WATCH_PERIOD=0` or the AI tab.

## Not measured

One machine; the Chernarus+ p2 row is a different phase of the mission than PERF-019's rows
(combat had started), so only p1 is directly comparable. Whether 0.5 s is *felt* — a head that
turns half a second late — is a playtest question and belongs to the owner.
