# REN-VEG-002 — The LOD governor does not see the GPU, and when it does it overshoots

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** REN-VEG-001 (retaining Chernarus+'s trees: `land:obj` 7.65 → 1.05 ms, GPU 30 → 49 ms)

## Why the retained trees cost ten GPU milliseconds

Not because the GPU cull picks LODs differently — it runs the same `dist² · lodInvWidth² · Left²`
against the same level resolutions as `Scene::LevelFromDistance2`, and it receives the live
`_lodInvWidth` every frame (the cull.wgsl comment "fixed, not fed back" is stale; the value is
pushed from the presentation snapshot). Every DayZ tree registers all four visual LODs
(resolutions 1, 2, 3, 4 at ~4k / 2k / 1k / 6 triangles — `WGR_INSTANCE_TRACE=1`, 120 plant
models). The histogram still says level 0 for 5,940 of 6,055 instances.

The reason is the **governor's estimate**. `Scene::AdjustComplexity()` steers `_lodInvWidth` from
the summed complexity of the objects the CPU is about to draw. An object diverted to the
retained path leaves `_drawObjects`, so its faces leave that sum: retaining 4,300 trees made the
frame look *cheaper* to the governor, which kept the detail multiplier fine, and the GPU —
reading that same multiplier — drew them at level 0. And the governor's target band is
`1000/(frameRate·2/3) .. 1000/(frameRate·4/3)` with `frameRate` defaulting to **15** (the
owner's `UserInfo.cfg` says 15.0): it does not consider a 55 ms frame slow at all.

## What was tried, and measured (Chernarus+, WLD-025 pose, one run per arm, 800×600)

`WGR_LOD_GOVERNOR_RETAINED=1` adds the previous frame's retained main-view triangle count
(the cull's own readback, same units as `GetComplexity`) to the governor's sum.

| arm | CPU frame | fps | GPU | main-view tris | LOD histogram (0..4) |
|---|---:|---:|---:|---:|---|
| bail ON (old default) | 31.1 | 32 | 30.3 | 3.8 M | 1513, 109, 0, 0, 0 |
| retained, governor blind | 55.3 | 18 | 49.4 | 30.5 M | 5971, 84, 0, 0, 0 |
| retained + governor term | 55.7 | 18 | 57.7 | 30.5 M | 5940, 115, 0, 0, 0 |
| retained + term + `WGR_LOD_GOVERNOR_RANGE=64` | **9.7** | **103** | **9.5** | 0.2 M | 19, 232, 197, **3516**, 261 |

Two findings, one per row:

1. **The term alone changes nothing on Chernarus+** because `_lodInvWidth` is clamped at
   `_maxLodInvWidth = quality · range` (range 16) — the governor was already at its coarseness
   bound. Widening the bound is what let it act.
2. **When it acts, it overshoots.** Range 64 took the frame from 55 to 9.7 ms and the trees to
   level 3 (the 6-triangle card). The picture (`.tmp-veg/aerial_old_retained_range64.png`, right
   panel) shows why that is not a result: the distant hills fill with pale blobs — the coarsest
   tree LOD renders as untextured cards on the retained path — and the whole scene has dropped a
   level. `Scene.cpp:619` already names the mechanism: the range is both the clamp *and* the
   target-line geometry of the feedback loop, so widening the clamp moves the operating point.

On Everon the term worked as designed and that was equally wrong: the whole retained set dropped
a level (2.3 → 0.2 M triangles, GPU 28 → 21 ms) on a frame that was not GPU-bound, with no
frame-time gain to show for it.

## Decision

Both levers stay opt-in: `WGR_TREEADV_RETAINED=1` (trees on the GPU path) and
`WGR_LOD_GOVERNOR_RETAINED=1` (the GPU term). The parity fixes of REN-VEG-001 ship. The capture
JSON now carries `objects.lod_inv_width`, so the next run of this can quote the multiplier
instead of inferring it from a histogram.

## What has to happen, in order

1. **Decouple the governor's clamp from its target** (the fix `Scene.cpp:619` already
   prescribes) and re-derive the target band from a modern frame-rate setting — 15 fps is a
   2001 default and makes the loop inert on every machine the owner tests on.
2. **Fix the coarsest tree LOD on the retained path** (pale untextured cards at level 3–4; the
   per-draw path draws the same level correctly, so it is a material/section classification
   difference, and `WGR_INSTANCE_TRACE` plus `SECTRACE` will name the section).
3. Then re-run the four arms above. The prize is the fourth row's frame time with the first row's
   picture; nothing less ships as a default.
