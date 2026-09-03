# REN-SKY-003 — The interior sky-visibility maps are cached per layer: 0.94 → 0.00 ms at rest

**Date:** 2026-09-03
**Branch:** `asset-obs-001`

## The mechanism

The interior sky-visibility maps (five 2048² top-down depth layers over a 64 m box around the
camera, the ambient-occlusion-by-roofs term) were re-culled and re-drawn every frame: 0.28 ms
of cull and 0.66 ms of depth draws on Everon `perf_combat` at 3441×1440. They depend on exactly
two things — the camera position, already snapped to the map texel so the map cannot crawl —
and the retained instance set. Nothing else moves them: the directions are fixed in world
space, terrain is not in them, and the sun does not enter.

So each layer now remembers the view it was drawn with and the instance-table epoch (a
counter bumped on every add, update or remove in the cull's `InstanceTable`). A layer is stale
when its fresh snapped view differs from the drawn one or the epoch moved, and at most **one
stale layer is re-culled and re-drawn per frame**, round-robin. The forward shader and the
cull keep sampling with the views the layers were actually drawn with, so a layer drawn
five frames ago at a slightly different origin is still read correctly; the box only sits a
few metres off-centre. A layer that has never been drawn (first frame, resolution change)
forces all five that frame, so nothing undrawn is ever sampled.

## Measured (Everon `perf_combat`, 3441×1440, three alternating runs per arm, stamp `769ba635+dirty`)

| | every frame (old) | **cached** |
|---|---:|---:|
| Interior sky: cull | 0.29 (0.28–0.31) | **0.00** |
| Interior sky: depth maps | 0.65 (0.64–0.66) | **0.00** |

The bench camera stands still, so this is the resting cost: nothing is stale, nothing is
drawn. Moving, or with the streamer admitting objects every update, the bound is one layer
per frame — a fifth of the old cost, ≤0.19 ms — and that bound is by construction, not
measured here. The GPU frame total was noisy in both arms of this session (17.1–21.2 ms
either way), so it is not quoted.

## The picture

Full-frame difference between one run of each arm: 0.48% of pixels differ by more than 32
levels, 0.018% by more than 96 — grass and cloud motion. The roof and porch darkening is
the same map, drawn once instead of every frame.

## Decision

**ON by default** (`WGR_INTERIOR_SKY_CACHE=0` redraws all five layers every frame). The lag
is at most five frames on a layer that moved, on an ambient term with no hard edges.

## Not measured

A moving-camera capture. The `.tmp-movecam` harness exists for that if a lag is ever
reported; the expected signature would be a roof's ambient darkening arriving a few frames
after the roof itself, which is below what an eye picks up at 60 Hz.
