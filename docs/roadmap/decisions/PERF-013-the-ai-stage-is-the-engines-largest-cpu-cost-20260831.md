# PERF-013 — Inside the simulation loop: the AI stage is the engine's largest CPU cost

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Follows:** PERF-012 (the fixed-step loop is 48% of a combat frame)
**Roadmap:** step 14 (*optimise measured hotspots*), Phase 8 step 12S (*parallelise measured
AI workloads*)

PERF-012 opened a phase called `setup` and found the whole fixed-step simulation loop inside
it, at 30 ms — the largest single CPU cost in the engine — and could say nothing about which
part. `RunSimStagesSerial` (SIM-811) already walks the six stages one at a time, so it is the
one place a per-stage cost can be taken without a timer at every call site. It now does.

## The measurement

`perf_combat`, Everon, 110 s warm-up so the object stream has settled, six consecutive
one-second windows. Milliseconds **per tick**:

| stage | ms/tick | share |
|---|---:|---:|
| **AI** (`PerformAI`) | **21.1 – 34.3** | ~60–70% |
| Vehicles (`SimulateAllVehicles`) | 9.8 – 22.2 | ~25–35% |
| Visibility (`SmartUpdateAll`) | 1.4 – 3.2 | ~4% |
| Clock (`SimulateLandscape`) | 0.05 – 0.09 | 0.2% |
| Scripts | 0.000 | off by default (`POSEIDON_FIXEDSTEP_SCRIPTS`) |
| Physics | 0.000 | no physics world in this mission |
| **total** | **34.7 – 55.6** | |

**The AI stage is the engine's largest CPU cost, by a factor of two over the next one.**

## Two things this corrects

**The `ai+veh` phase was reporting 0.16 ms for work that costs 30–55 ms.** Its mark sits
inside the region the fixed-step loop already covered, so it measured a sliver after the loop
rather than the loop's AI. Anyone reading the profiler would have concluded the AI was free.
Both halves of the misreading — a bucket that hid the loop, and a phase that appeared to
measure it — were in the same profiler and pointed the same wrong way.

**The simulation is not keeping up with real time.** Each window covers one second of
accumulated `deltaT` and holds fewer ticks than the 60 the fixed step nominally wants — 32 in
the window this table came from, and **40 to 59 across the eight windows the addendum
measures**, which is the range to quote. The accumulator is shedding ticks because a tick
costs more than its budget. So "ms per tick" and "ms per second of play" are not the same
number here, and the table above is the former. The addendum below counts what is actually
lost.

## What follows

- **Step 14's hotspot is named.** Not "the simulation": `PerformAI`, at 21–34 ms/tick. SIM-803
  already found real defects one level in (a ranking sorted by heap address, a comparator
  containing a cycle) and SIM-812 mapped its write surface; neither was looking for cost.
- **It is also Phase 8's best parallel candidate on evidence rather than expectation**, which
  is what step 12S asks for. Whether it *can* be parallelised is a separate question that
  SIM-812's audit bears on (AI neighbour writes go past setters via friend access, so there is
  no chokepoint to intercept).
- **It does not change PERF-012's conclusion about overlap.** A render thread hides
  simulation behind the GPU wait; it does not make the AI cheaper. With 30–55 ms of tick
  against 27 ms of `swap`, the wait is the smaller number and remains the ceiling.

## The instrument

Timing lives in `RunSimStagesSerial`, one `steady_clock` pair per stage, six per tick against
stages costing milliseconds. Unconditional on purpose: no gate to forget, and no way for a
zero to mean "did not run" — a stage that is skipped is absent from the trace, and one that
ran and cost nothing reads 0.000, which is what `scripts` and `physics` legitimately are here.
Reported once a second with its tick count, because a per-stage mean without the denominator
is unreadable.

Wall time, and diagnostic only: it must never reach a simulation decision. SIM-813 removed the
last wall clock that did, and SIM-816's determinism mode would report a divergence if one came
back — which is a real check, not a hope.

## What this does not say

- One scene, one machine, six windows. `perf_combat` is the only one of the five perf missions
  with meaningful AI load; on `perf_abel` the whole loop is 0.15 ms.
- It does not say what *inside* `PerformAI` costs 30 ms. That is the next subdivision.
- Physics reads 0.000 because this mission has no physics world, not because physics is free.

---

## Addendum — the world runs slow in a fight, and nothing was counting

PERF-013 noted in passing that a one-second window held 32 ticks against a nominal 60, and
left it as a caveat about reading the table. It is worth more than a caveat.

`FixedStepAccumulator::Advance` caps a frame at 8 catch-up steps and then **discards the
remainder** (`_accumulatedSeconds = fmod(...)`). That is the anti-spiral guard doing exactly
what it was written for: a 200 ms frame cannot be allowed to queue twelve ticks and make the
next frame worse. But a discarded step is simulation that never happens — world time thrown
away — and the class counted neither the steps nor the frames it happened in. It does now
(behaviour unchanged; two counters and three accessors).

`perf_combat`, 110 s warm-up, consecutive one-second windows:

```
59 ticks in the last second (nominal 60);  discarded  8 steps in 1 frame
40 ticks                                   discarded 21 steps in 4 frames
58 ticks                                   discarded  4 steps in 3 frames
55 ticks                                   discarded  8 steps in 5 frames
40 ticks                                   discarded 26 steps in 5 frames
40 ticks                                   discarded 23 steps in 5 frames
40 ticks                                   discarded 23 steps in 3 frames
40 ticks                                   discarded 35 steps in 5 frames
```

**In sustained combat the tick rate settles at 40 of 60 and the cap throws away 23–35 steps
a second** — between a third and six tenths of a second of world time, every second, in five
frames or fewer. The game world is running at roughly two thirds speed, and in the worst
window closer to half.

This is not a bug in the accumulator: without the cap those frames would compound. It is the
*cost* of the AI stage arriving somewhere nobody was looking. A player in a heavy fight sees
the world in slow motion, and until this line existed there was no way to say so with a
number, or to tell it apart from "the renderer is slow".

It also sharpens what fixing the AI stage would buy. Making `PerformAI` twice as fast would
not just raise the framerate: it would give back the world time the cap is currently
discarding, which is a correctness-shaped improvement rather than a smoothness one.

**What this does not say.** One scene, one machine, one run of eight windows. The cap of 8 is
`DefaultMaxCatchUpSteps` and nothing here argues it is the right number — raising it trades
slow motion for hitching, and that trade is not measured. And a low tick rate with *zero*
discards would mean something entirely different (long frames, no lost time), which is why
the line reports both.
