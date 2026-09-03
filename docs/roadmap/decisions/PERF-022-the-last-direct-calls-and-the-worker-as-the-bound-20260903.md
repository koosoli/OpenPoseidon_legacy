# PERF-022 — The last direct calls, and the worker as the new bound

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-021 (overlap was lockstep)

## What the opener log said, in order

With overlap real, REN-THR-013's window opener finally had something to name. Everon
`perf_combat`, native, VSync pinned off:

1. **`water cascade config`, every frame** — `WaterWgpu::DrawWater` re-applied the cascade
   preset each frame (five `wgr_water_set_cascade_config` calls, "per-frame by design" so the
   live seed override rides along), and each opened the window: `land:gnd` 6.8 ms, of which
   ~6 was the wait. The push is now deduplicated on its arguments. The four other per-frame
   water pushes (`wgr_water_set_params`, interaction params, interaction events, planar pad)
   were direct and unguarded too; they are queued into `PendingRendererSettings` like the rest.
2. **`gpu timings`, every frame** — a timer-row reader runs every frame (the Perf tab's HUD
   path), and `GetWaterGpuTimings` opened the window for it: 1.00 lazy waits and 7.8 ms per
   frame. The worker now snapshots the GPU and CPU timer rows after each frame
   (`PublishRendererStats`) and every reader gets the snapshot.

After both: **0.02 lazy waits per frame** (`texture create` on the odd cold admission), `land:gnd`
6.8 → 0.35 ms.

## Where the frame is now (three runs)

| | PERF-021 fixed | **after PERF-022** |
|---|---:|---:|
| CPU frame avg | 21.9 | 22.3 (21.6–22.8) |
| fps | 45.6 | 44.9 |
| `swap` (ring wait) | 3.6 | 9.2 |
| worker busy per frame | (unmeasured) | **18.9** |
| GPU frame | 19.5 | 21.7 (18.4–23.4) |

The frame did not move because the bound moved: with nothing left to wait for on the producer,
the producer now waits in the **ring** for the worker, whose block is 18.9 ms per frame — up
from 7.4 measured under lockstep and 12.6 under the first real overlap. The worker's encode
slows as the producer runs beside it (the contention PERF-018 first measured), and the encode
itself is largely unattributed: the bracketed regions' CPU column sums to ~1.5 ms of it.

Two instruments landed for that: every GPU region bracket now also clocks the CPU that
recorded it (`cpu_begin`/`cpu_end` ride `begin`/`end`/`begin_pass`/`end_pass`), and the worker
frame has five CPU-only rows (`Worker: swapchain acquire / present / frame setup / queue submit /
readback harvest`). Setup is 1.8–3.2 ms; the rest of the ~10–15 ms encode sits in code no
bracket names yet. The render-thread stats window is wall time now; it never closed inside a
capture before, which is why the capture JSON read zeros for the worker fields.

Also landed (PERF-023): the Vehicles simulation stage subdivided by sub-stage and by entity
kind. `perf_combat`: `slow` 3.82 of 3.91 ms/tick, and of that **`man` 3.54 ms/tick over 193
soldier Simulate calls** — 18 µs per soldier per tick; everything else (tanks, cars, air,
statics, buildings, cloudlets) is under 0.2 ms together.

## Not done, by the owner's decision

The worker's encode attribution and the soldier Simulate cost are the next two CPU rows and
are parked: the owner asked for the lighting system (hybrid GI) instead of further small
optimisations, and for the cloud light march to keep its four-tap picture (REN-SKY-002's coarse
march is now opt-in, `WGR_CLOUD_LIGHT_COARSE=1`).
