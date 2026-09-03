# PERF-024 — The worker's encode, attributed: recording draw calls is 6.6 ms

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-022 (the worker became the frame's bound, ~10–15 ms of it unattributed)

## The instrument

Seven CPU-only timer rows now cover `render_frame` end to end, disjoint and in order:

| row | what it spans |
|---|---|
| `Worker: frame setup` | entry to `begin_frame` — uploads, drains, the probe/dome prepares, **and the swapchain acquire** |
| `Worker: swapchain acquire` | `get_current_texture`, inside the above |
| `Worker enc: early` | grass/water updates, skin bake, the culls, interior sky, GI probes |
| `Worker enc: shadows` | the cascade passes |
| `Worker enc: sky` | atmosphere LUTs, froxel, env bake, cloud march, the sky pass |
| `Worker enc: draw` | the depth prepass and the 3D colour sub-pass replay, water included |
| `Worker enc: post` | the post chain, upscale, UI |
| `Worker enc: tail` | the stats/mip/visibility resolves |
| `Worker: queue submit` / `present` | the two calls |

They are CPU columns on regions whose GPU column reads n/a, so they sit in the same table as
every other region and in the capture JSON.

## Measured (Everon `perf_combat`, 3441×1440, VSync off, two runs)

| row | run 1 | run 2 |
|---|---:|---:|
| frame setup (incl. acquire) | 2.81 | 10.88 |
| ⤷ swapchain acquire | 0.03 | 7.97 |
| enc: early | 0.08 | 0.25 |
| enc: shadows | 0.38 | 0.54 |
| enc: sky | 0.03 | 0.04 |
| **enc: draw** | **6.83** | **6.42** |
| enc: post | 0.00 | 0.01 |
| enc: tail | 0.00 | 0.00 |
| queue submit | 0.80 | 0.41 |
| present | 0.06 | 0.05 |

Two readings:

1. **Recording the draws is the worker's CPU cost.** 6.4–6.8 ms of the ~10.5 ms of real CPU
   work is the prepass plus the 3D colour sub-pass replay. Everything else together is under
   1 ms; the post chain and the tail are free. That matches WLD-026's finding from the other
   side: 77% of visible objects never register on the GPU-driven path, so they are replayed one
   `Draw3D` op at a time, and each op re-binds and re-issues.
2. **The rest of the worker's block is GPU back-pressure, not work.** `swapchain acquire` was
   0.03 ms in one run and 7.97 in the next, on the same scene: with the present queue full the
   worker waits there. Setup's own work is ~2.8 ms in both.

## What this names for the roadmap

The next lever on the CPU side is **fewer recorded draws**, i.e. raising GPU-driven coverage
(the `GpuModel foliage bail` lines WLD-026 counted), not micro-optimising the replay. The
second is `Person::Simulate` on the producer, 18 µs per soldier per tick (PERF-023).

Nothing here is a change to what the game draws: this decision adds instrumentation only.
