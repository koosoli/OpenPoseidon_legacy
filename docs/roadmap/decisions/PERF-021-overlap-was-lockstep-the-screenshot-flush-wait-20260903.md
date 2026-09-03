# PERF-021 — Overlap was lockstep: the screenshot flush waited for the worker every frame

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-THR-012 (the producer window), REN-THR-013 (the lazy window)

## How it was found

REN-THR-013's A/B read nothing on every scene, and the explanation "the scenes are GPU-bound"
did not survive the next instrument. Three CPU-only timer rows were added to the worker
(`Worker: swapchain acquire`, `Worker: present`, later `frame setup`, `queue submit`,
`readback harvest`), and the worker loop learned to report how long it sat idle before each
block (`idle_before=` on the `WGR_SLOW_BLOCK_MS=1` line). Everon `perf_combat`, native,
VSync pinned off, one run each:

| where the worker's frame went | ms |
|---|---:|
| swapchain acquire | 0.04 |
| frame setup (uploads, drains) | 1.78 |
| encode (the bracketed regions sum to <1 ms; the rest is unbracketed encode) | ~5 |
| queue submit | 0.41 |
| present | 0.16 |
| readback harvest | 0.04 |
| **block total** | **7.4** |
| **idle before the block** | **13.3 (p50 10.0)** |

The worker never waits for the GPU (acquire and present are microseconds) and is idle for
more than half of every frame. The producer, meanwhile, spends ~7.5 ms per frame in a wait
that the NextFrame breakdown printed as `slotWait` — but the ring-slot predicate cannot block
while the worker is idle, and its printed value tracked the *previous* frame's whole duration,
so it was not the ring. It was `_rtPreDrawWaitMs`, the `WaitRenderIdle` accumulator.

`FlushPendingScreenshot()` runs every frame from NextFrame, right after publish. REN-THR-012
put `WaitRenderIdle()` as its first line, ahead of the "is a screenshot pending" check. So the
producer handed the worker a frame and then immediately waited for the worker to finish it —
every frame, screenshot or not. Overlap mode has been lockstep with an extra thread since
2026-09-02, which is exactly what REN-THR-012's own table said ("overlap equals lockstep
here, within spread") and attributed to the GPU.

## The fix

The pending check first; the wait only when there is a screenshot to take.
`WGR_FLUSH_WAIT_EAGER=1` keeps the old order for the A/B.

## Measured (Everon `perf_combat`, 3441×1440, VSync pinned off, three alternating runs per arm)

| | wait first (old) | **check first** |
|---|---:|---:|
| CPU frame avg | 23.8 (23.4–24.5) | **21.9** (21.6–22.4) |
| CPU frame p95 | 26.9 | **21.0** |
| fps | 42.0 | **45.6** |
| `swap` (contains NextFrame) | 12.0 | **3.6** |
| `sim:step` | 6.6 | 6.2 |
| `drw:land` / `land:obj` | 2.1 / 1.4 | 3.2 / 2.0 |
| GPU frame | 17.7 | 19.5 (one 22.2 outlier) |

+8.5% frames, and the p95 drops below the old average: the frame no longer has the worker's
block serialised into it. The traversal phases grow by ~1.7 ms because they now actually run
beside the worker (the same core contention PERF-018 measured for `sim:step`); that is the
price of overlap, paid once and already netted in the frame figure. Picture identical (0.03%
of pixels differ by more than 96 levels; grass and cloud motion).

The producer is the bound now: `sim` 6.2 + traversal ~6 + NextFrame ~3.6 + the rest ≈ 22 ms
against a ~19 ms GPU frame. That reorders the roadmap: the CPU-side levers (REN-THR-013's
lazy window, the traversal's own cost, NextFrame's 3.6 ms) are on the critical path for the
first time on this machine, and the GPU levers no longer are on this scene.

## Why the earlier reasoning went wrong twice

- REN-THR-012 measured overlap == lockstep and blamed the GPU: the GPU frame (21–26 ms) was
  close to the CPU frame (29–30), so the story fit. It was never checked against a worker
  idle figure, because there was none.
- REN-THR-013 measured nothing on three scenes and again said "GPU-bound" (`swap` 11–15 ms).
  `swap` is the phase that *contains* NextFrame, and NextFrame contained the wait.

The rule this adds to Phase 3.1: **a wait must be named at the call site it happens in, not
inferred from a phase that contains it.** The worker idle row and the `AcquireProducerWindow`
opener are that kind of instrument; `swap` is not.
