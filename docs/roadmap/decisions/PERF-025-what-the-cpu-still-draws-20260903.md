# PERF-025 — What the CPU still draws: 2,572 footprints and 1,100 road pieces per frame

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-024 (recording the draws is 6.6 ms of the worker's frame)

## The instrument

The pass-1 line has counted `coverage none=` for a while — the objects the GPU-driven path does
not take, which the CPU therefore replays one `Draw3D` op at a time. It never named one, so the
number was not actionable. `WGR_COVERAGE_NONE_CENSUS=1` now tallies them **by model** and prints
the worst offenders once a second with the denominator.

Its own denominator was wrong on the first run and said so loudly: "271219 draws in 271219
frames (1/frame)" — the frame counter was incremented in the per-draw path. It is incremented in
`Scene::BeginObjects` now, which runs once per frame.

## Measured (Everon `perf_combat`, noon, one run)

`coverage full=15477 (78.3%) partial=562 none=3736`, and the census says what "none" is —
4,105–4,406 CPU-drawn objects per frame from **38 distinct models**:

| model | draws/frame | what it is |
|---|---:|---|
| `data3d\stopa_l.p3d` | 1,380 | left footprint |
| `data3d\stopa_p.p3d` | 1,192 | right footprint |
| `asfatlka*`, `silnice*`, `cesta*` (8 models) | ~1,100 together | road segments |
| `mc vojakw2/e2` | ~170 | map/corpse markers |
| the remaining 27 models | the rest | |

**Footprints are 60% of the CPU complement.** Two models, 2,572 draw calls a frame, each a
small quad. Roads are the next 27%.

Both are `Offset::Decal` — OnSurface geometry, which is deliberately excluded from the
prepassed and GPU-driven paths (REN-OBJ's `prepassed()` and the road pixel-conform work), so
they will not join the retained set by accident.

## The lever this names

One instanced draw per decal model instead of one draw per decal. They share a model, a
material and a pipeline; only the transform differs, which is exactly what an instanced run is
for. A cap on live footprints would also work but changes what the player sees; batching does
not.

Nothing here changes what the game draws: this decision adds the census only.
