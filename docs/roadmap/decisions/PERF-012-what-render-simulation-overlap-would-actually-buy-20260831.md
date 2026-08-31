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
256-frame ring; simulation-side is `setup` + `ai+veh` + `sound`, and `swap` is the present
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

**`setup` is a bucket, not a subsystem.** It is the whole pre-draw phase, which on a
streaming world includes the object stream's admission. During Everon's fill-in `setup`
reaches 51 ms; the runs above were taken at 110 s, after the mutation counters read zero for
tens of frames, so the fill-in is done — but "simulation-side" here still means "everything
before the draw", not "AI and physics".

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
