# REN-THR-012 — The producer window: the render thread shipped with a data race, and what closing it costs

**Date:** 2026-09-02
**Branch:** `asset-obs-001`, commit `df9db1f0`
**Follows:** REN-THR-001 (the exclusive-handle blocker), PERF-018 (what overlap bought),
`ownership-authority.md` rows 3.7–3.10

## The finding

REN-THR-001 counted 62 of 79 `wgr_*` entry points that form `&mut *renderer` — an exclusive
Rust reference — and set the design: producer-side mutations become queued ops the worker
applies, so the worker alone touches the renderer while it renders. The queues shipped for
mesh, model, texture (overlay), instance and seven `wgr_set_*` settings. **They did not ship
for everything.** Still called directly by the producer, every frame, inside the draw
traversal and `NextFrame` itself:

| producer call | from | Rust side |
|---|---|---|
| `wgr_terrain_set_params` | `TerrainWgpu::DrawTerrain` | `&mut` |
| `wgr_set_render_params`, `wgr_set_smoke_shadow`, cloud/star/lens | `PushRenderParams` (NextFrame line ~2502) | `&mut` |
| `wgr_set_sky_runtime` | `PushSkyRuntime` (same place) | `&mut` |
| `wgr_far_set_instances` | `SetFarProxyInstances` | `&mut` |
| `wgr_texture_slot_acquire`, `wgr_texture_destroy`, `_slot_release` | `TextureWgpu` on cold admission and on death | `&mut` |
| `wgr_get_mip_feedback`, stats readers, `wgr_get_temporal_info` | texture bank, dev panel | `&` |
| `wgr_screenshot_request/take`, `wgr_resize`, `wgr_set_present_mode`, shadow dumps | user events, harness | `&mut` |

Under overlap (`WGR_RENDER_THREAD` + `WGR_RENDER_OVERLAP`, both default ON since 2026-09-01)
the producer waited only for its ring **slot** before building the next frame — so all of
the above ran while the worker was inside `wgr_render_frame`, which also holds `&mut`. That
is undefined behaviour in Rust's aliasing model and, concretely, a torn `render_params`
struct or a far-instance buffer replaced mid-encode. The header still said "MAIN THREAD
ONLY". Nothing could report it: REN-THR-002's oracle hashes the *published frame* (producer
data), not the renderer's state; the owner's evening on the build showed no crash, which is
what a race usually shows.

The ownership table said "T1 only" for rows 3.7–3.10 the whole time. The render thread's
commit did not edit it. That is the process failure; the race is its consequence.

## The fix

A boundary, not a lock. `EngineWgpu::WaitRenderIdle()` at the top of `InitDraw` blocks the
producer until the worker holds no frame; the window closes at publish when the worker is
handed the next one. The simulation — whose renderer mutations are already queued instance
ops — still overlaps the render; the draw traversal, which calls directly, runs after it. The
rare direct callers that can run outside the window (screenshot, resize, vsync, MSAA, shadow
dumps, far instances) wait for themselves. The wait is folded into the Perf tab's *Queue
waiting*.

The window is exposed as `EngineWgpu::ProducerMayTouchRenderer()`, and the two texture paths
that reach the renderer outside the queues (destroy, slot acquire) check it: a violation logs
three times and counts. Row 3.9 of the ownership table is a **checked** rule now. The ABI
header's contract is rewritten to the one that holds: one thread at a time, in frame order.

## What it costs — measured, and what the measurement can and cannot say

`perf_combat`, bench window size, RTX 3070, three runs per arm, same binary `eeb532fd`:

| arm | CPU frame avg (3-run mean) | p95 | `sim:step` | `swap` | `drw:init` |
|---|---:|---:|---:|---:|---:|
| overlap ON (default, with the wait) | 30.04 ms | 31.31 | 11.03 | 14.75 | 0.050 |
| `WGR_RENDER_OVERLAP=0` (lockstep) | 29.23 ms | 31.79 | 7.15 | 15.16 | 0.048 |

- **The wait itself is free on this scene.** `InitDraw` is billed to `drw:init`
  (`World.cpp:1693` sits between the `setup` and `drw:init` marks), and `drw:init` is 0.05 ms
  in both arms: by the time the producer finishes its simulation step the worker is already
  idle. The +3.9 ms in `sim:step` under overlap is the core contention PERF-018 already
  measured (+84% then), not the wait.
- **Overlap equals lockstep here, within spread.** The scene is GPU-bound at this window
  size — GPU frame 21.7–26.4 ms of a 29–30 ms CPU frame — and the producer spends ~14.7 ms in
  `swap` waiting for the worker to *claim* a frame in both modes, because the worker is
  blocked in present. A GPU-bound frame has nothing for overlap to hide.
- **What is not measured:** PERF-018's 22% came from a CPU-bound run (43.5 → 33.9 ms). How
  much of that survives the wait on a CPU-bound scene is the open number; this bench could
  not produce that condition (mission mode does not honour `-Width/-Height`). Expect the
  simulation's share to survive and the traversal's share (`drw:*` + `land:*`, ~5 ms on
  Everon) to be lost.

## What follows

Winning the traversal's share back means queueing the remaining direct calls the way the
instance and mesh ops already are: render params, sky runtime and terrain params as fields
of `PendingRendererSettings`; far instances as a resource op; the texture slot lease as a
producer-side allocation (the uploader borrow pattern of REN-THR-009). When every traversal
call is queued, the wait moves from `InitDraw` to publish and the window becomes the whole
frame again — this time honestly. Row 3.10's work list carries it.
