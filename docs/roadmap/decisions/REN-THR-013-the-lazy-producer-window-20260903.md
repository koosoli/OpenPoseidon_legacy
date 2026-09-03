# REN-THR-013 — The lazy producer window: the traversal stops waiting for the worker

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-THR-012 (the producer window: `InitDraw` waits for the render thread)

## What changed

REN-THR-012 closed a data race by making `InitDraw` wait until the worker holds no frame,
because the draw traversal still called the renderer directly. That serialised the whole
traversal (`drw:*` + `land:*`, 4–5 ms on Everon) behind the worker. This slice removes the
reason for the wait rather than the wait:

- **The per-frame pushes are queued.** Render params (tonemap, exposure, sky look, GTAO,
  interior sky, DoF), the sky runtime, the terrain params, the smoke-shadow blobs and the three
  sky scalars now go into `PendingRendererSettings` and are applied by the worker at the top of
  its block, before `wgr_render_frame` — the same frame they used to be pushed into, one thread
  later. `WaitRenderIdle()` applies the pending set on return, so the stop-the-world callers
  (MSAA, resize, screenshots) still read a renderer that reflects everything pushed.
- **The producer-side memory readers use the worker's snapshot.** The residency trim, the
  memory probe and the terrain residency log read `EngineWgpu::LastMemoryStats()`; only an
  over-budget frame opens the window and reads the live figure before evicting.
- **Every remaining direct call acquires the window itself.** `AcquireProducerWindow(what)`
  returns at once while the worker is idle and otherwise waits — and records who asked. Texture
  create/update/destroy and the slot lease, the mip-feedback harvest, the terrain and grass
  uploads, the photo-clump query, the timing and exposure readers all go through it. The window
  therefore opens on the first call that needs it, or not at all.
- `InitDraw` no longer waits. `WGR_LAZY_WINDOW=0` restores REN-THR-012's eager wait.

The Perf tab shows "Window opened late … by <what>" and the capture JSON carries
`cpu_frame_phases_ms.producer_window` (`lazy_wait_ms`, `lazy_waits_per_frame`, `opener`), so
a scene that still opens the window early names the caller.

## What the measurement first said, and why it was wrong

The first A/B (Everon `perf_combat`, 3441×1440, three alternating runs per arm) read 23.5 vs
22.7 ms CPU frame — inside the spread. The run logs carried "VSync interval set to 1": the
bench runs the owner's saved config, VSync on, on a 144 Hz display, so every CPU frame ends on
a 6.94 ms boundary and `frame_avg` is a mixture of 20.8 and 27.8 ms frames. `POSEIDON_VSYNC=0`
now pins the interval for a run (logged); Phase 3.1 gained the rule.

## Measured with VSync off (three alternating runs per arm, stamp `b5847aa6+dirty`)

| scene | arm | CPU frame avg | GPU frame | `swap` | lazy waits / frame |
|---|---|---:|---:|---:|---:|
| Everon `perf_combat`, native | eager (THR-012) | 23.2 (22.4–24.5) | 19.8 | 11.4 | 0 |
| | **lazy** | 24.2 (23.8–24.5) | 19.9 | 11.6 | **0.00** |
| Everon `perf_town`, native | eager | 16.3 | 14.3 | 14.9 | 0 |
| | **lazy** | 16.3 | 15.7 | 14.9 | **0.00** |

**Both stock scenes at native resolution are GPU-bound** — the producer spends 11–15 ms in
`swap` waiting for the worker, which is waiting for the GPU — so the traversal's share was
never on the critical path there and the lazy window changes nothing, as REN-THR-012 predicted
for a GPU-bound frame. What the table does establish: on these scenes **the traversal never
touches the renderer** (0.00 lazy waits per frame; the only recorded opener is the bench's
own GPU-timings reader at capture time), which is the precondition the earlier decision named.

CPU-bound pose: see the addendum below (Chernarus+ ground, world mode 800×600).

## Decision

**Lazy window ON by default.** It is a strict superset of the eager one: a frame with no
direct call overlaps fully; a frame with one waits exactly where the eager path would have
waited for everything. The `opener` field is the instrument for the next slice — whichever
caller still opens the window on a streaming world (texture create on cold admission is the
expected one) is what the queued-texture work of REN-THR-009's list should take next.

## Not measured

Chernarus+ under streaming load with a moving camera, where cold-texture creation opens the
window mid-traversal; the addendum's pose is static.

Also open: the capture JSON's `worker_busy_ms` / `producer_wait_ms` / `overlap_ms` (added
with this slice from `RenderThreadInfo`) read 0 in every run while the same fields drive the
Perf tab; the "slow frame" log line prints a non-zero `block=` from the same source. Until that
is understood the worker's period is read from `WGR_SLOW_BLOCK_MS=1` logs, not from the JSON.
