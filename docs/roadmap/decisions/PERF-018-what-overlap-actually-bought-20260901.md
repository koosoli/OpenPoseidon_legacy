# PERF-018 — What render/simulation overlap actually bought

> ## ⚠ CORRECTION, same day: the 318 ms "spike" is this harness photographing itself
>
> This document made `frame_max` its most emphatic point — 318 ms before overlap, 318 ms after,
> "the largest unexplained cost in the engine". **It is the screenshot.**
>
> `FlushPendingScreenshot()` runs at the end of `EngineWgpu::NextFrame`, so the readback is
> booked into the `swap` phase of the frame that takes it. The harness takes exactly one, at the
> end of the run — and `cpu_frame_phases_ms` is a 256-frame ring that also ends there. So the
> capture frame is inside the ring of **every** measurement this harness has ever produced, and
> `frame_max` has always been that frame.
>
> Proven by instrumenting the whole of `NextFrame`: `whole=298.5 ms | slotWait=27.1 block=33.7
> trim=0.0 invalidate=0.1 rest=271.3`, and the log line sits between "wgpu screenshot readback
> completed" and "AUTO-TEST SUCCESS", 14 lines from the end of a 2774-line log.
>
> **Withdrawn:** "the spike is the largest unexplained cost in the engine", and the whole of
> point 2 under *What follows*. There is no evidence here of a recurring 300 ms hitch, and the
> claim that overlap "did nothing for the worst frame" compared two screenshots.
>
> **Unaffected:** the mean and p95 columns, and every conclusion drawn from them. Those are
> ordinary frames — 43.50 → 33.91 ms and 46.97 → 36.07 ms — and the three-runs-per-arm ranges
> still do not overlap.
>
> **The rule that follows:** `frame_max` out of `--capture-metrics` is not a spike metric and
> never was. Percentiles up to p95 are usable; the maximum is the instrument's own shutter.
> This is the same family as PERF-013's load-window error, in a costume nobody recognised:
> the window did not include the load this time, it included the photograph.


**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Follows:** PERF-012 (predicted ceiling), REN-THR-016 (overlap landed and proven equivalent)
**Roadmap:** critical-path step 9, *"Enable bounded, measurable render/simulation overlap"*

PERF-012 measured a **ceiling** of 34–42% on `perf_combat` and was explicit that it was a
ceiling: handoff cost, queue serialisation and cache effects all subtract and none was
estimated. REN-THR-016 then landed overlap and proved it publishes the same frames as the
inline path — 901 of 901, aligned by simulation tick. This is the part neither of them did:
what it is worth.

## The measurement

`perf_combat`, 1600×900, `WGR_DLSS=0`, screenshot at frame 3000 so the 256-frame phase ring
covers steady play rather than the load. Three runs per arm, alternated, **idle machine** — the
first attempt at this table was taken while a build and a test suite were running and was
discarded (PERF-013's rule, and the second time this session it has had to be applied).

| | inline (3 runs) | overlap (3 runs) | |
|---|---:|---:|---:|
| frame mean | 43.50 ms | **33.91 ms** | **−22.0%** |
| frame p95 | 46.97 ms | **36.07 ms** | **−23.2%** |
| **frame max** | 318.22 ms | **318.01 ms** | **−0.1%** |
| `swap` | 29.10 ms | 9.11 ms | −69% |
| `sim:step` | 7.74 ms | 14.21 ms | **+84%** |
| avg fps | 23.0 | 29.5 | +28% |

Run-to-run ranges do not overlap: inline 41.84–45.68 ms, overlap 32.75–34.58 ms.

## Three readings, and the last two matter more than the first

**It delivered about half its ceiling.** 22% against a predicted 34–42%. That is the handoff
cost PERF-012 said it could not estimate, and it is worth recording as a rough conversion rate
for the next such prediction rather than treating 22% as a disappointment.

**The spikes did not move at all.** 318 ms before, 318 ms after — three tenths of one percent,
which is nothing. Bohemia's OPREP for Arma 3 2.20 says their goal was fewer spikes and higher
minimum framerate rather than higher mean, and PERF-016 took that as a metric this project
should adopt. Here is why, from the opposite direction: this change improved the mean by 22%
and the worst frame by nothing. A player feels the 318 ms frame. **Whatever causes it is not
the GPU wait, and overlap was never going to help it** — that is the next question, and it is
not this one.

**The simulation got more expensive per frame, by 84%.** `sim:step` rises from 7.74 to
14.21 ms. This is not a regression in the simulation: with overlap the producer is no longer
parked in `swap`, so it runs against a render thread that is now competing for the same cores
and the same memory bandwidth. It is the cost side of the trade, it is real, and it is why the
22% is not the 43% that `swap` alone would suggest.

## What follows

1. **Overlap is worth having on the scene it was built for**, and PERF-012's warning stands:
   `perf_abel` has a 1.3% ceiling and would show none of this. Report overlap on `perf_combat`
   or do not report it.
2. **The spike is now the largest unexplained cost in the engine.** 318 ms against a 34 ms
   mean, unchanged by the one thing that halved the GPU wait. `FixedStepAccumulator`'s
   discarded-step counter (PERF-013) fires on exactly these frames and nothing has yet read it
   as a spike instrument.
3. **1% and 0.1% lows belong in the bench.** The harness already reports p95 and max; nothing
   reports them habitually, and this table is the argument that it should.

## What this does not say

- One scene, one machine, one resolution, three runs per arm.
- It does not attribute the 318 ms frame to anything. It says only that overlap did not touch
  it.
- `sim:step` rising is measured, and the *explanation* (core and bandwidth contention) is
  inferred, not measured. Nobody has pinned threads or counted cache misses here.
- The AI ration of PERF-017 was NOT enabled for these runs. Both are changes to the same frame
  and their interaction is unmeasured.
