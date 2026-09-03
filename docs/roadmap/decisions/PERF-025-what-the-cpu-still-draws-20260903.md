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

**Footprints are 60% of the CPU complement.** Two models, 2,572 classified objects a frame,
each a small quad. Roads are the next 27%.

**Which counting point is which** — three numbers on the same frame, and only one of them is a
draw call:

| number | where it is counted | Everon `perf_combat` |
|---|---|---:|
| `coverage none` / this census | `Scene::ObjectForDrawing`, per object classified | 3,736 / 4,105 |
| `direct_calls` (renderer) | the CPU-replayed `Draw3D` ops the worker records | **3,566** |
| `submit counts: objDraws` | the `_drawMergers` submit path only | 270 |

The submit-path counter is small because marks and roads never enter `_drawMergers`; they are
drawn from the landscape/mark path, which is also why the instanced-run batching there reports
`runs=0` for them and its veto counters barely move. The draw-call number to quote is
`direct_calls`: **3,566 CPU draw calls a frame**, and PERF-024 measured recording them at 6.6 ms.

## Where the footprints come from

`SoldierOldMove` spawns a `Mark` entity per footfall with a **10 second** life
(`GScene->Preloaded(FootStepL/R)`, radius 0.25 m, added to the world's animal list). With ~190
soldiers walking, a few thousand are alive at any moment, and each is one object and one draw.

Both are `Offset::Decal` — OnSurface geometry, which is deliberately excluded from the
prepassed and GPU-driven paths (REN-OBJ's `prepassed()` and the road pixel-conform work), so
they will not join the retained set by accident.

## The lever this names

One instanced draw per decal model instead of one draw per decal. They share a model, a
material and a pipeline; only the transform differs, which is exactly what an instanced run is
for. The existing run batcher cannot do it for two reasons, both to be dealt with: it vetoes
`Routing::OnSurface` outright, and marks do not pass through it at all.

Two cheaper alternatives change what the player sees and are therefore the owner's call, not
mine: a cap on live marks, and a shorter life than 10 s.

Nothing here changes what the game draws: this decision adds the census only.
