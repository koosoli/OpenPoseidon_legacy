# PERF-012 — What render/simulation overlap would actually buy, per scene

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** critical-path step 9, *"Enable bounded, measurable render/simulation overlap"*

Step 9 says **bounded and measurable**. Phase 5's handoff work is nearly done — four drain
slices landed and the equivalence oracle reaches zero — so the honest question to ask before
building a render thread is the one the step's own wording implies: **how much is there to
win, and where?** This measures it, and the answer is "almost nothing on three of four
scenes, and about a third of the frame on the fourth".

## Method

Overlap does not remove the GPU wait. It lets the simulation run *during* it. So the ceiling
on what a render thread can recover is

```
ceiling = min(simulation-side CPU work, time spent in swap)
```

per frame, and it is a **ceiling**: real handoff costs, and any serialisation the queues
impose, come out of it. Phases from `--capture-metrics`'s `cpu_frame_phases_ms`, which is a
256-frame ring; simulation-side is `setup` + `ai+veh` + `sound` (and, after the addendum below, `sim:step`), and `swap` is the present
call. Two runs per scene, `WGR_DLSS=0`, 1600×900.

## Result

| scene | CPU frame (ring mean) | swap | simulation-side | **overlap ceiling** |
|---|---:|---:|---:|---:|
| `perf_abel` | 11.79 ms | 10.74 ms (91%) | 0.15 ms | **0.15 ms — 1.3%** |
| `perf_town` | 6.06 ms | 4.93 ms (81%) | 0.46 ms | **0.46 ms — 7.6%** |
| `perf_combat`, run 1 | 49.21 ms | 26.88 ms (55%) | 16.73 ms | **16.73 ms — 34.0%** |
| `perf_combat`, run 2 | 57.36 ms | 27.78 ms (48%) | 23.84 ms | **23.84 ms — 41.6%** |

**On the bench scenes there is nothing to win.** `perf_abel` spends 91% of its CPU frame in
`swap` with 0.15 ms of simulation behind it: it is GPU-bound, exactly as PERF-011 reported,
and a render thread would recover a tenth of a millisecond. `perf_town` is the same story.

**On the combat scene there is a third of the frame.** 16.7–23.8 ms of simulation work sits
in front of a 27 ms GPU wait. That is the case a render thread is for, and it is also the
case nobody has been benchmarking: every performance figure in this project's history —
PERF-010, PERF-011, the whole grass campaign — was measured on `perf_abel`, the scene where
this particular change is worth nothing.

## Two cautions on these numbers

**The ring mean is not the steady frame.** The harness reports `perf_combat`'s *cheapest
steady* frame as 13.9 ms while the 256-frame ring averages 49 ms — the scene has heavy
variance, and the ring includes the spikes. The ceiling is a fraction of the mean, so it is
a claim about the *average* frame, which is the right one for a throughput change, but it is
not a claim that a 14 ms frame contains 17 ms of recoverable work.

**`setup` was a bucket, and it has been opened — see the addendum.** It was the whole
pre-draw phase, and its own comment in `FrameProfiler.hpp` listed "input, network, scripts,
camera, landscape sim" without mentioning that the entire fixed-step simulation loop ran
inside it.

## What follows for the plan

1. **Finish the handoff, but stop justifying it with framerate on `perf_abel`.** The
   remaining drain work (CPU-path `mesh_create`/`set_skin`, far-proxy upload, the stats
   reads) is worth doing because it makes the boundary *true*, not because it makes the
   bench scene faster. It will not.
2. **Measure step 9 on `perf_combat`, and add it to the standard arm set.** A change that
   recovers 30% of a combat frame and 1% of a field frame will look like noise if it is only
   ever measured on the field.
3. **The GPU-bound scenes want a different lever.** 91% of `perf_abel`'s CPU frame is waiting
   for the GPU; nothing on the CPU side can help it. That is a statement about where the
   *next* performance work should go, and it is not this phase.

## What this does not say

- Two runs per scene, one machine, one resolution. The ratio is stable between the two
  combat runs (34% and 42%) but the absolute frame time is not.
- It does not measure what overlap would actually *achieve* — only its ceiling. Handoff cost,
  the drain's own serialisation, and cache effects all subtract, and none is estimated here.
- `perf_water` and `phys_things` were not run.


---

## Addendum — the bucket opened: it is the simulation loop, 48% of the combat frame

`PhaseSetup` was marked at `World.cpp:1490`, and `StepSimulation` is called in its loop at
`World.cpp:916` — several hundred lines earlier. So the entire fixed-step simulation ran
inside a phase named "setup" whose comment did not mention it. `perf.Mark` accumulates
(`+=`), so closing the bucket before the loop and opening a `sim:step` phase after it splits
the two without changing what either measures.

Same scene, same 110 s warm-up, `perf_combat`:

| phase | avg | share of frame |
|---|---:|---:|
| **`sim:step`** | **30.08 ms** | **48.3%** |
| `swap` | 27.02 ms | 43.3% |
| `drw:land` | 2.32 ms | 3.7% |
| `land:obj` | 1.43 ms | 2.3% |
| everything else together | 1.49 ms | 2.4% |
| `setup`, now that it is only what its name says | **0.13 ms** | 0.2% |

So the largest single CPU cost in the engine is the fixed-step simulation loop, at half the
combat frame, and until this split it was reported under a name that suggested
initialisation. `ai+veh` is 0.16 ms — that phase is marked *inside* the region the loop
already covers, so it never measured the AI's real cost either.

This sharpens the overlap ceiling rather than changing it: simulation-side is now
30.4 ms against 27.0 ms of `swap`, so the ceiling is the **swap** (43.3% of the frame), not
the simulation. There is more simulation work than there is GPU wait to hide it behind.

And it points step 14 ("optimise measured hotspots") at a specific place. Nothing here says
*what* inside `StepSimulation` costs 30 ms — SIM-810 lists its six stages and that is the
next subdivision, not this one.
